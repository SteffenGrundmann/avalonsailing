// Copyright 2011 The Avalon Project Authors. All rights reserved.
// Use of this source code is governed by the Apache License 2.0
// that can be found in the LICENSE file.

#include <cstdlib>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <errno.h>
#include <termios.h>

#include "io/nmea/nmea_parser.h"
#include "io/nmea/nmea_interpretations.h"
#include "io/ipc/producer_consumer.h"
#include "lib/fm/fm.h"
#include "lib/fm/log.h"
#include "lib/util/reader.h"
#include "lib/util/strings.h"

using std::map;
using std::pair;
using std::string;
using std::vector;

struct SupportedNMEASentences {
  const char* identifier;
  NmeaInterpreter interpreter;
} kSupportedNmeaSentences[] = {
  { "WIMWV", WIMWVInterpreter },
  { "WIXDR", WIXDRInterpreter },
  // TODO(rekwall): AIS comes here.
  { "", NULL}  // Sentinel.
};

const long kReadTimeout = 5000;  // 5 seconds.

// Map of NMEA type (e.g. "WIMWV") to the function that interprets that type
// and the Producer that writes the interpreted data to a file.
typedef pair<NmeaInterpreter, Producer> InterpreterAndProducer;
typedef map<string, InterpreterAndProducer> InterpreterMap;

namespace {
void InterpretSentence(const NmeaSentence& sentence,
                       const InterpreterMap& interpreters) {
  const string& sentence_type = sentence.parts[0];
  InterpreterMap::const_iterator found = interpreters.find(sentence_type);
  if (found == interpreters.end()) {
    FM_LOG_FATAL("Received NMEA sentence of type '%s' for which there "
                 "is no registered interpreter.", sentence_type.c_str());
  }

  NmeaInterpreter interpreter = found->second.first;
  const Producer& producer = found->second.second;

  string output;
  if (!(*interpreter)(sentence, &output)) {
    // TODO(rekwall): more details about sentence here.
    FM_LOG_FATAL("Failed to interpret sentence of type '%s'",
                 sentence_type.c_str());
  }
  if (!producer.Produce(output)) {
    FM_LOG_FATAL("Failed to output '%s'", output.c_str());
  }
}

void ReadNmeaSentences(Reader* reader, const InterpreterMap& interpreters) {
  NmeaParser parser;
  NmeaSentence sentence;

  FM_LOG_INFO("Starting to read NMEA sentences.");
  char buf[200];
  while (reader->ReadLine(buf, 200, kReadTimeout) == Reader::READ_OK) {
    switch(parser.Parse(buf, &sentence)) {
     case NmeaParser::CHECKSUM_MISSING:
     case NmeaParser::MALFORMED_CHECKSUM:
       // TODO(rekwall): log more details.
       FM_LOG_WARN("NMEA sentence dropped: invalid checksum data.");
       break;
     case NmeaParser::INCORRECT_CHECKSUM:
       FM_LOG_WARN("NMEA sentence dropped: incorrect checksum.");
       break;
     case NmeaParser::INCORRECT_START_CHAR:
       FM_LOG_WARN("NMEA sentence dropped: unexpected start character.");
       break;

     case NmeaParser::SENTENCE_PARSED:
       // Find sentence.parts[0] in the type -> function map.
       printf("%s\n", sentence.DebugString().c_str());
       InterpretSentence(sentence, interpreters);
       // TODO(rekwall): this shouldn't be called too often. For windsensor
       // data, it will be called about once a second. This should be guarded by
       // a check on the elapsed time since last call though.
       FM::Keepalive();
       break;
     default:
       FM_LOG_FATAL("Unknown NMEA parser status.");
       break;
    }
  }
}

// Opens @tty_path (at a speed of @baudrate) and reads NMEA sentences. The
// supported sentences have a key in @interpreters and are interpreted by
// the functions in the @interpreters values.
void StartNmeaDaemon(const string& tty_path, int baudrate,
                     const InterpreterMap& interpreters) {
  while (true) {
    Reader tty_reader;
    if (tty_reader.OpenSerial(tty_path.c_str(), baudrate)) {
      ReadNmeaSentences(&tty_reader, interpreters);
      FM_LOG_WARN("NMEA sentence reader returned. This could mean that the "
                  "underlying device isn't responding anymore");
    } else {
      FM_LOG_FATAL("Could not open tty %s at baud rate %d.",
                   tty_path.c_str(), baudrate);
    }
  }
}

// Adds the interpreter for @nmea_idenfier to the map of supported NMEA
// sentences.
bool AddInterpreter(const string& nmea_identifier,
                    const string& output_path,
                    InterpreterMap* interpreters) {
  for (int i = 0; kSupportedNmeaSentences[i].interpreter != NULL; ++i) {
    if (nmea_identifier.compare(kSupportedNmeaSentences[i].identifier) == 0) {
      interpreters->insert(
          make_pair(nmea_identifier,
                    make_pair(kSupportedNmeaSentences[i].interpreter,
                              Producer(output_path))));
      return true;
    }
  }
  return false;
}
}  // anonymous namespace

int main(int argc, char **argv) {
  // Get index at which daemon's parameters start.
  int arg_start = FM::Init(argc, argv);

  if (argc - arg_start < 3) {
    FM_LOG_FATAL("Usage: %s "
                 "<tty> <baudrate> (<nmea-identifier>:<output-file>)+",
                 argv[0]);
  }

  const string device_name(argv[arg_start]);

  int speed = -1;
  {
    // TODO(rekwall): factor this out to lib/util.
    char* endptr = NULL;
    errno = 0;
    speed = strtol(argv[arg_start + 1], &endptr, 0);
    if (endptr[0] != '\0' || errno == ERANGE) {
      FM_LOG_FATAL("Could not convert %s to an integer baud rate.",
                   argv[arg_start + 1]);
    }
  }

  InterpreterMap interpreters;
  for (int i = arg_start + 2; i < argc; ++i) {
    vector<string> nmea_interpreter;
    SplitString(argv[i], ':', &nmea_interpreter);
    if (nmea_interpreter.size() != 2) {
      FM_LOG_FATAL("Expected <nmea-identifier>:<output-file> for "
                   "argument %d. Found '%s' instead.",
                   i, argv[i]);
    }
    if (!AddInterpreter(nmea_interpreter[0],  // NMEA identifier.
                        nmea_interpreter[1],  // Output path.
                        &interpreters)) {
      FM_LOG_FATAL("Unsupported NMEA identifier: '%s'.",
                   nmea_interpreter[0].c_str());
    }
  }

  FM_LOG_INFO("Starting NMEA daemon.");
  StartNmeaDaemon(device_name, speed, interpreters);
}
