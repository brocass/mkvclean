#include "sdh.h"

#include <QRegularExpression>

namespace {

// Sound descriptions: [DOOR CLOSES], (sighs). Non-greedy within one line; a
// bracket that spans lines is left alone rather than risking the rest of the
// cue.
const QRegularExpression kSquareBrackets(QStringLiteral("\\[[^\\]]*\\]"));
const QRegularExpression kRoundBrackets(QStringLiteral("\\([^\\)]*\\)"));

// Speaker labels: "JOHN:", "- MAN:", ">> NARRATOR:", "MAN (V.O.):". Only an
// all-uppercase prefix is treated as a label, so dialogue such as "He said:
// no" keeps its words.
const QRegularExpression kSpeakerLabel(
    QStringLiteral("^\\s*(?:[-–—]\\s*|>>\\s*)?[A-Z][A-Z0-9 .'’#\\-]{0,29}:\\s*"));

// A formatting tag left with nothing inside it, e.g. "<i></i>".
const QRegularExpression kEmptyTag(
    QStringLiteral("<([a-zA-Z][a-zA-Z0-9]*)(?:\\s+[^>]*)?>\\s*</\\1>"));

const QRegularExpression kMusicSymbols(QStringLiteral("[♪♫♬♩]"));
const QRegularExpression kHasText(QStringLiteral("[\\p{L}\\p{N}]"));
const QRegularExpression kBlankLine(QStringLiteral("\\n[ \\t]*\\n+"));

QString cleanLine(const QString &raw, bool *removed)
{
    QString line = raw;
    line.remove(kMusicSymbols);
    line.remove(kSquareBrackets);
    line.remove(kRoundBrackets);

    const auto label = kSpeakerLabel.match(line);
    if (label.hasMatch())
        line.remove(0, label.capturedLength());

    // Collapse the gaps the removals left behind ("He turns   away.").
    line = line.simplified();

    for (int pass = 0; pass < 4 && kEmptyTag.match(line).hasMatch(); ++pass)
        line.remove(kEmptyTag);
    line = line.trimmed();

    if (!kHasText.match(line).hasMatch()) {
        if (removed)
            *removed = true;
        return QString();
    }
    return line;
}

}

Sdh::CleanResult Sdh::cleanSrt(const QString &input)
{
    CleanResult result;
    QString text = input;

    const bool hadBom = text.startsWith(QChar(0xFEFF));
    if (hadBom)
        text.remove(0, 1);
    const bool crlf = text.contains(QStringLiteral("\r\n"));
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    QStringList outputBlocks;
    const auto blocks = text.split(kBlankLine, Qt::SkipEmptyParts);
    for (const QString &block : blocks) {
        const QStringList lines = block.split(QLatin1Char('\n'));

        int timing = -1;
        for (int i = 0; i < lines.size(); ++i) {
            if (lines.at(i).contains(QStringLiteral("-->"))) {
                timing = i;
                break;
            }
        }

        // Not a cue (a WEBVTT header, a malformed block): preserve it so the
        // conversion never silently discards text it does not understand.
        if (timing < 0) {
            if (!block.trimmed().isEmpty())
                outputBlocks.append(block);
            continue;
        }

        ++result.cues;
        QStringList keptLines;
        for (int i = timing + 1; i < lines.size(); ++i) {
            bool removed = false;
            const QString line = cleanLine(lines.at(i), &removed);
            if (line.isEmpty()) {
                if (removed)
                    ++result.removedLines;
                continue;
            }
            keptLines.append(line);
        }

        if (keptLines.isEmpty())
            continue;
        ++result.kept;
        outputBlocks.append(QString::number(result.kept) + QLatin1Char('\n')
                            + lines.at(timing) + QLatin1Char('\n')
                            + keptLines.join(QLatin1Char('\n')));
    }

    result.text = outputBlocks.join(QStringLiteral("\n\n"));
    if (!result.text.isEmpty())
        result.text += QLatin1Char('\n');
    if (crlf)
        result.text.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
    if (hadBom)
        result.text.prepend(QChar(0xFEFF));
    return result;
}
