
#include "base/record_replay_paint_surface.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/page/focus_controller.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/core/record_replay/lifecycle.h"
#include "third_party/blink/renderer/platform/wtf/casting.h"

namespace recordreplay {

static std::string page_description(blink::Page* page) {
    std::string main_frame_info;

    if (auto* main_local_frame = blink::DynamicTo<blink::LocalFrame>(page->MainFrame())) {
        main_frame_info = main_local_frame->GetDocument()->Url().GetString().Utf8();
    } else {
        main_frame_info = "remote!?!?";
    }

    std::stringstream desc;
    desc << "Page(" << main_frame_info << ")";
    return desc.str();
}

void NotePageVisibilityStateChanged(blink::Page* page) {
    std::string page_desc = page_description(page);
    std::stringstream to;
    to << page->GetVisibilityState();
    recordreplay::Print("%s visibility changed to %s\n", page_desc.c_str(), to.str().c_str());
    // LOG(ERROR) << page_desc << " visibility changed to " << to.str();

    // When a page is made visible, signal that the paint surface is about to change,
    // so we don't ignore the paints.
    if (page->GetVisibilityState() == blink::mojom::blink::PageVisibilityState::kVisible) {
        recordreplay::DoResetPaintSurface();
    }
}

void NotePageFocusControllerActiveChanged(blink::Page* page) {
    std::string page_desc = page_description(page);
    bool active = page->GetFocusController().IsActive();
    recordreplay::Print("%s focus controller changed to %s\n", page_desc.c_str(), active ? "active" : "inactive");
    // LOG(ERROR) << page_desc << " focus controller changed to " << (active ? "active" : "inactive");
}

} // namespace recordreplay
