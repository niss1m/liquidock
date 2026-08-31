#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace liquidock {

// One line of editable text, with a caret and a selection.
//
// The fields in this window were a buffer and an index: no selection, no way to
// click into the middle of a word, no Ctrl+A, and a caret that could only be
// moved by deleting back to where you wanted to be. That is fine for a value
// nobody edits twice and wrong for a path, which is the longest and most fiddly
// thing anyone types here.
//
// This holds the string, where the caret is, where a selection started, and how
// far the text has slid to keep the caret in view. Measuring is the caller's
// job - it owns the font and the device - so nothing here knows what a pixel
// is, and every field in the window can share one implementation of what the
// keys do.
class TextField {
public:
    void Set(std::wstring text);
    void Clear();
    const std::wstring& text() const { return text_; }
    bool empty() const { return text_.empty(); }
    size_t size() const { return text_.size(); }

    size_t caret() const { return caret_; }
    size_t anchor() const { return anchor_; }
    bool has_selection() const { return caret_ != anchor_; }
    size_t selection_begin() const { return caret_ < anchor_ ? caret_ : anchor_; }
    size_t selection_end() const { return caret_ < anchor_ ? anchor_ : caret_; }
    std::wstring selected() const;

    void SelectAll();
    // The run of like characters around `index` - a word, or the run of
    // punctuation between two words, which is what a double-click means.
    void SelectWordAt(size_t index);
    // `extend` keeps the anchor where it was, which is what holding shift does.
    void PlaceCaret(size_t index, bool extend);

    // Each returns true when the text itself changed, so a caller can tell a
    // move apart from an edit without comparing strings.
    bool Insert(std::wstring_view text);
    bool Backspace(bool wholeWord);
    bool DeleteForward(bool wholeWord);
    bool DeleteSelection();

    void MoveLeft(bool extend, bool wholeWord);
    void MoveRight(bool extend, bool wholeWord);
    void MoveHome(bool extend);
    void MoveEnd(bool extend);

    // How far the text is slid left, in whatever units the caller measures in.
    // Kept here rather than beside the rectangle that draws it: a field that
    // forgets where it was scrolled to jumps back to the start every time the
    // panel is laid out again, which is every keystroke.
    float scroll() const { return scroll_; }
    void SetScroll(float scroll) { scroll_ = scroll; }

    size_t WordLeft(size_t from) const;
    size_t WordRight(size_t from) const;

private:
    void Clamp();

    std::wstring text_;
    size_t caret_ = 0;
    size_t anchor_ = 0;
    float scroll_ = 0.0f;
};

} // namespace liquidock
