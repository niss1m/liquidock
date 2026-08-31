#include "ui/TextField.h"

#include <algorithm>
#include <cwctype>

namespace liquidock {
namespace {

// What a text field means by a word: letters, digits and the underscore. A path
// separator is not one, which is what makes Ctrl+Left step through a path one
// folder at a time rather than jumping to the front of it.
bool IsWordChar(wchar_t ch) {
    return iswalnum(ch) != 0 || ch == L'_';
}

} // namespace

void TextField::Set(std::wstring text) {
    text_ = std::move(text);
    caret_ = text_.size();
    anchor_ = caret_;
    scroll_ = 0.0f;
}

void TextField::Clear() {
    text_.clear();
    caret_ = 0;
    anchor_ = 0;
    scroll_ = 0.0f;
}

void TextField::Clamp() {
    caret_ = std::min(caret_, text_.size());
    anchor_ = std::min(anchor_, text_.size());
}

std::wstring TextField::selected() const {
    if (!has_selection()) {
        return {};
    }
    return text_.substr(selection_begin(), selection_end() - selection_begin());
}

void TextField::SelectAll() {
    anchor_ = 0;
    caret_ = text_.size();
}

void TextField::PlaceCaret(size_t index, bool extend) {
    caret_ = std::min(index, text_.size());
    if (!extend) {
        anchor_ = caret_;
    }
}

void TextField::SelectWordAt(size_t index) {
    if (text_.empty()) {
        anchor_ = caret_ = 0;
        return;
    }
    index = std::min(index, text_.size() - 1);
    // Three kinds of run, so that double-clicking a word takes the word,
    // double-clicking the gap takes the gap, and neither takes both.
    const wchar_t here = text_[index];
    const auto same = [here](wchar_t ch) {
        if (iswspace(here)) {
            return iswspace(ch) != 0;
        }
        return IsWordChar(here) == IsWordChar(ch) && iswspace(ch) == 0;
    };
    size_t begin = index;
    while (begin > 0 && same(text_[begin - 1])) {
        --begin;
    }
    size_t end = index;
    while (end < text_.size() && same(text_[end])) {
        ++end;
    }
    anchor_ = begin;
    caret_ = end;
}

bool TextField::DeleteSelection() {
    if (!has_selection()) {
        return false;
    }
    const size_t begin = selection_begin();
    text_.erase(begin, selection_end() - begin);
    caret_ = begin;
    anchor_ = begin;
    return true;
}

bool TextField::Insert(std::wstring_view text) {
    // Typing over a selection replaces it, which is the whole reason a
    // selection is worth having.
    DeleteSelection();
    if (text.empty()) {
        return false;
    }
    Clamp();
    text_.insert(caret_, text);
    caret_ += text.size();
    anchor_ = caret_;
    return true;
}

bool TextField::Backspace(bool wholeWord) {
    if (DeleteSelection()) {
        return true;
    }
    Clamp();
    if (caret_ == 0) {
        return false;
    }
    const size_t from = wholeWord ? WordLeft(caret_) : caret_ - 1;
    text_.erase(from, caret_ - from);
    caret_ = from;
    anchor_ = from;
    return true;
}

bool TextField::DeleteForward(bool wholeWord) {
    if (DeleteSelection()) {
        return true;
    }
    Clamp();
    if (caret_ >= text_.size()) {
        return false;
    }
    const size_t to = wholeWord ? WordRight(caret_) : caret_ + 1;
    text_.erase(caret_, to - caret_);
    anchor_ = caret_;
    return true;
}

void TextField::MoveLeft(bool extend, bool wholeWord) {
    Clamp();
    // An unshifted arrow with something selected collapses to the near end
    // rather than stepping from the caret. Every text field does this, and it
    // is the difference between arrowing out of a selection and arrowing into
    // the middle of one.
    if (!extend && has_selection() && !wholeWord) {
        caret_ = selection_begin();
        anchor_ = caret_;
        return;
    }
    caret_ = wholeWord ? WordLeft(caret_) : (caret_ > 0 ? caret_ - 1 : 0);
    if (!extend) {
        anchor_ = caret_;
    }
}

void TextField::MoveRight(bool extend, bool wholeWord) {
    Clamp();
    if (!extend && has_selection() && !wholeWord) {
        caret_ = selection_end();
        anchor_ = caret_;
        return;
    }
    caret_ = wholeWord ? WordRight(caret_) : std::min(caret_ + 1, text_.size());
    if (!extend) {
        anchor_ = caret_;
    }
}

void TextField::MoveHome(bool extend) {
    caret_ = 0;
    if (!extend) {
        anchor_ = 0;
    }
}

void TextField::MoveEnd(bool extend) {
    caret_ = text_.size();
    if (!extend) {
        anchor_ = caret_;
    }
}

size_t TextField::WordLeft(size_t from) const {
    size_t at = std::min(from, text_.size());
    while (at > 0 && iswspace(text_[at - 1])) {
        --at;
    }
    if (at == 0) {
        return 0;
    }
    const bool word = IsWordChar(text_[at - 1]);
    while (at > 0 && !iswspace(text_[at - 1]) && IsWordChar(text_[at - 1]) == word) {
        --at;
    }
    return at;
}

size_t TextField::WordRight(size_t from) const {
    size_t at = std::min(from, text_.size());
    if (at >= text_.size()) {
        return text_.size();
    }
    const bool word = IsWordChar(text_[at]);
    while (at < text_.size() && !iswspace(text_[at]) && IsWordChar(text_[at]) == word) {
        ++at;
    }
    while (at < text_.size() && iswspace(text_[at])) {
        ++at;
    }
    return at;
}

} // namespace liquidock
