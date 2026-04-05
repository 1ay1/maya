#pragma once
// maya::components::FeedbackButtons — Thumbs up/down rating
//
//   FeedbackButtons fb;
//   fb.update(ev);
//   auto ui = fb.render();
//   if (fb.rating() != Rating::None) { /* handle */ }

#include "core.hpp"

namespace maya::components {

enum class Rating { None, ThumbsUp, ThumbsDown };

struct FeedbackButtonsProps {
    bool  show_text_on_downvote = true;
    Color up_color              = palette().success;
    Color down_color            = palette().error;
};

class FeedbackButtons {
    Rating      rating_           = Rating::None;
    std::string feedback_text_;
    bool        editing_feedback_ = false;
    FeedbackButtonsProps props_;

public:
    explicit FeedbackButtons(FeedbackButtonsProps props = {})
        : props_(std::move(props)) {}

    [[nodiscard]] Rating rating() const { return rating_; }
    [[nodiscard]] const std::string& feedback_text() const { return feedback_text_; }
    [[nodiscard]] bool editing() const { return editing_feedback_; }

    void update(const Event& ev);

    [[nodiscard]] Element render() const;
};

} // namespace maya::components
