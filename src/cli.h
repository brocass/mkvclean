#pragma once

#include <QStringList>

class QCoreApplication;

// `mkvclean clean <file-or-folder>… [--json]`: the whole job with no display.
// One file prints a single result; several print one result per file (JSONL
// with --json). Errors go to stderr; exit 0 when everything was cleaned.
int runClean(QCoreApplication &app, const QStringList &inputs, bool json);

// `mkvclean watch …`: manage and run the folder-watch service.
// Subcommands: run (default), add, remove, list, enable, disable, status.
int runWatch(QCoreApplication &app, const QStringList &args);

// `mkvclean skill install`: drops the bundled agent skill next to Omarchy's.
int runSkillInstall();
