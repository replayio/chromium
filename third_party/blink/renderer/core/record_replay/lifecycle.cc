
#include "base/record_replay_paint_surface.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/page/focus_controller.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/core/record_replay/lifecycle.h"
#include "third_party/blink/renderer/platform/wtf/casting.h"

#define LOG_LIFECYCLE_EVENTS 1

namespace recordreplay {

// the current set of ordinary pages, presumably scoped to this renderer process (TODO verify this)
// lives at blink::Page::OrdinaryPages()

// There might be a reference to our last-active page someplace in blink already, but until/unless we find it:
static blink::Page* last_active_page = nullptr;

static void set_last_active_page(blink::Page* page) {
    if (page == last_active_page) {
        // it's already the active page
        return;
    }

#if LOG_LIFECYCLE_EVENTS
    recordreplay::Print("  - updating last-active page\n");
#endif
    last_active_page = page;
    // signal that the paint surface is about to change, so we don't ignore
    // the paints to the surface corresponding to our now active page.
    recordreplay::DoResetPaintSurface();
}

static void reset_last_active_page() {
#if LOG_LIFECYCLE_EVENTS
    recordreplay::Print("  - clearing last-active page\n");
#endif
    last_active_page = nullptr;
}

#if LOG_LIFECYCLE_EVENTS
static std::string page_description(blink::Page* page) {
    std::string main_frame_info;

    if (auto* main_local_frame = blink::DynamicTo<blink::LocalFrame>(page->MainFrame())) {
        main_frame_info = main_local_frame->GetDocument()->Url().GetString().Utf8();
    } else {
        main_frame_info = "<unknown>";
    }

    std::stringstream desc;
    desc << "Page(" << main_frame_info << ")";
    return desc.str();
}
#endif

void NotePageVisibilityStateChanged(blink::Page* page) {
#if LOG_LIFECYCLE_EVENTS
    std::string page_desc = page_description(page);
    std::stringstream to;
    to << page->GetVisibilityState();
    recordreplay::Print("%s visibility changed to %s\n", page_desc.c_str(), to.str().c_str());
#endif

    // When a page is focused and is made visible, it becomes our last-active page.
    if (page->GetFocusController().IsActive() &&
        page->GetVisibilityState() == blink::mojom::blink::PageVisibilityState::kVisible) {

        set_last_active_page(page);
    }
}

void NotePageFocusControllerActiveChanged(blink::Page* page) {
#if LOG_LIFECYCLE_EVENTS
    std::string page_desc = page_description(page);
    recordreplay::Print("%s focus controller changed to %s\n", page_desc.c_str(), page->GetFocusController().IsActive() ? "active" : "inactive");
#endif

    // When a page visible and then focused, it becomes our last-active page.
    if (page->GetFocusController().IsActive() &&
        page->GetVisibilityState() == blink::mojom::blink::PageVisibilityState::kVisible) {

        set_last_active_page(page);
    }

}

void NotePageWillBeDestroyed(blink::Page* page) {
#if LOG_LIFECYCLE_EVENTS
    std::string page_desc = page_description(page);
    recordreplay::Print("%s will be destroyed\n", page_desc.c_str());
#endif

    if (page == last_active_page) {
        reset_last_active_page();
    }
}

} // namespace recordreplay
