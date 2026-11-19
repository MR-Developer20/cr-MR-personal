#pragma once

#include <Print.h>

#include <cstdint>
#include <string>
#include <vector>

// Streaming CommonMark-core -> XHTML converter. Feeds lib/Txt, which serves the result to the
// EPUB engine as the single spine item of a .md book. Reads the source and writes the output in
// small bounded chunks; never holds the whole document in RAM.
//
// This is a practical subset of CommonMark, not a full implementation. Simplifications,
// all chosen to keep the parser single-pass and bounded-memory while still covering the
// overwhelming majority of real-world Markdown:
//
//  * Headings are ATX only (`# Heading`). Setext headings (`Heading\n=====`) are not
//    recognized — the underline just renders as its own line/thematic-break — because
//    recognizing them needs one line of lookback after already having streamed the
//    preceding paragraph text out.
//  * No raw HTML passthrough: a literal '<', '>' or '&' in text is escaped and shown as-is
//    rather than interpreted as a tag.
//  * Blockquotes, list items, and code blocks don't nest inside each other — whichever
//    block type a line matches wins, and item/quote *content* is single-paragraph inline
//    text (no nested lists/quotes/code within an item).
//  * List nesting depth is derived from indentation width (~2 spaces per level), not a
//    full marker-alignment algorithm, and is capped at 4 levels.
//  * Ordered-list numbers are always auto-incremented from 1 at the start of each list run;
//    an explicit starting number in the source (e.g. "5. foo") is not honored.
//  * Inline emphasis/strong/code spans must open and close within the same source line —
//    they don't span a soft line break inside a paragraph.
//  * Only inline links/images are supported (`[text](url)`, `![alt](src)`); reference-style
//    links are not. Link targets are dropped (shown as plain text) since there's no browser
//    to follow them; images are resolved and embedded only when they're a local relative
//    JPEG/PNG file next to the source document.
class MarkdownToXhtml {
 public:
  struct Heading {
    uint8_t level;  // 1-6
    std::string text;
    std::string anchorId;  // "h0", "h1", ... in document order
  };

  struct Result {
    bool ok = false;
    std::string firstH1;  // raw text of the first top-level heading, empty if none
    std::vector<Heading> headings;
  };

  // Converts `srcPath` (a .md file on SD) into a complete XHTML document streamed to `out`.
  // `headerComment` (e.g. an HTML cache-version tag) is emitted right after the XML declaration
  // when non-null. Local JPEG/PNG images are referenced by their original relative path,
  // percent-encoded; the caller is responsible for serving them (see Txt::resolveCompanionResource).
  static Result convert(const std::string& srcPath, Print& out, const char* headerComment = nullptr);
};
