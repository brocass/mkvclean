#pragma once

#include <QString>

// Turns an SDH/CC SubRip file into a standard subtitle: sound descriptions,
// speaker labels and music symbols are removed, and cues that are left with no
// dialogue are dropped. The cue timings are kept exactly as they were.
namespace Sdh {

struct CleanResult {
    QString text;
    int cues = 0;         // cues found in the input
    int kept = 0;         // cues that still have dialogue
    int removedLines = 0; // text lines dropped as non-dialogue
};

CleanResult cleanSrt(const QString &input);

}
