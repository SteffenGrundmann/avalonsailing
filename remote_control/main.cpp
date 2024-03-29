// Copyright 2011 The Avalon Project Authors. All rights reserved.
// Use of this source code is governed by the Apache License 2.0
// that can be found in the LICENSE file.
// Author: jpilet@google.com (Julien Pilet)
//
// Entry point for the remote_control application.

#include <QtGui/QApplication>
#include <QString>
#include "mainwindow.h"
#include "clientstate.h"

int main(int argc, char *argv[])
{
  QApplication a(argc, argv);
  ClientState state;
  MainWindow w(&state);
  if (argc > 1) {
    state.setCommand(QString::fromAscii(argv[1]));
    state.tryToConnect();
  }
  w.show();
  return a.exec();
}
