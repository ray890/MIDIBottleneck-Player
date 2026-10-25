// The Help window: the eight topics of the portable Help text (app::help_text, app::parse_help_markup) in a GtkTextView with
// tags for headings, bold words, bullets and numbered steps.
#include "companion.h"

#include "midibn/app/help_text.h"
#include "midibn/app/ui_mappings.h"

namespace midibn {
namespace gtkui {

namespace {

class HelpWindow : public CompanionWindow {
public:
    explicit HelpWindow(CompanionHost& host) : host_(host)
    {
        window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(window_), (std::string(kProductName) + " Help").c_str());
        if (host.main_window) gtk_window_set_transient_for(GTK_WINDOW(window_), GTK_WINDOW(host.main_window));
        resize_for_scale();
        GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(outer), 8);
        gtk_container_add(GTK_CONTAINER(window_), outer);
        GtkWidget* paned = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(outer), paned, TRUE, TRUE, 0);

        list_ = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_), GTK_SELECTION_SINGLE);
        for (int i = 0; i < app::kHelpTopicCount; ++i) {
            GtkWidget* label = make_label(app::help_topic_title(static_cast<app::HelpTopic>(i)));
            gtk_widget_set_margin_start(label, 6);
            gtk_widget_set_margin_end(label, 12);
            gtk_widget_set_margin_top(label, 4);
            gtk_widget_set_margin_bottom(label, 4);
            gtk_list_box_insert(GTK_LIST_BOX(list_), label, -1);
        }
        GtkWidget* list_frame = gtk_frame_new(nullptr);
        gtk_container_add(GTK_CONTAINER(list_frame), list_);
        gtk_box_pack_start(GTK_BOX(paned), list_frame, FALSE, FALSE, 0);

        view_ = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(view_), FALSE);
        gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view_), FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view_), GTK_WRAP_WORD_CHAR);
        add_class(view_, "help-view");
        buffer_ = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view_));
        gtk_text_buffer_create_tag(buffer_, "heading", "weight", PANGO_WEIGHT_BOLD, "scale", 1.45, "pixels-below-lines", 6, nullptr);
        gtk_text_buffer_create_tag(buffer_, "sub", "weight", PANGO_WEIGHT_BOLD, "scale", 1.2, "pixels-above-lines", 8, "pixels-below-lines", 3, nullptr);
        gtk_text_buffer_create_tag(buffer_, "bold", "weight", PANGO_WEIGHT_BOLD, nullptr);
        gtk_text_buffer_create_tag(buffer_, "para", "pixels-below-lines", 6, nullptr);
        gtk_text_buffer_create_tag(buffer_, "bullet", "left-margin", 28, "indent", -14, "pixels-below-lines", 3, nullptr);
        gtk_text_buffer_create_tag(buffer_, "step", "left-margin", 32, "indent", -20, "pixels-below-lines", 3, nullptr);
        GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroller), GTK_SHADOW_IN);
        gtk_container_add(GTK_CONTAINER(scroller), view_);
        gtk_box_pack_start(GTK_BOX(paned), scroller, TRUE, TRUE, 0);

        GtkWidget* close_button = gtk_button_new_with_label("Close");
        gtk_widget_set_halign(close_button, GTK_ALIGN_END);
        gtk_box_pack_start(GTK_BOX(outer), close_button, FALSE, FALSE, 0);
        connect(close_button, "clicked", [this] { close(); });
        connect_arg(list_, "row-selected", [this] { on_row(); });
        connect(window_, "destroy", [this] { alive_ = false; window_ = nullptr; });
        connect_event(window_, "key-press-event", [this](GdkEvent* e) {
            if (reinterpret_cast<GdkEventKey*>(e)->keyval == GDK_KEY_Escape) { close(); return true; }
            return false;
        });
        gtk_widget_show_all(window_);
    }

    ~HelpWindow() override { close(); }

    GtkWidget* widget() const override { return window_; }
    void on_refresh() override {}
    void on_scale_changed() override { resize_for_scale(); }
    bool alive() const override { return alive_; }
    void close() override
    {
        if (alive_ && window_) {
            alive_ = false;
            GtkWidget* w = window_;
            window_ = nullptr;
            gtk_widget_destroy(w);
        }
    }

    void select(int topic)
    {
        if (topic < 0 || topic >= app::kHelpTopicCount) topic = 0;
        GtkListBoxRow* row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list_), topic);
        if (row) gtk_list_box_select_row(GTK_LIST_BOX(list_), row);
        if (window_) gtk_window_present(GTK_WINDOW(window_));
    }

private:
    void resize_for_scale()
    {
        if (!window_) return;
        const int percent = ScaleStyle::instance().percent();
        gtk_window_set_default_size(GTK_WINDOW(window_), app::scale_metric(760, percent), app::scale_metric(540, percent));
        if (gtk_widget_get_realized(window_)) gtk_window_resize(GTK_WINDOW(window_), app::scale_metric(760, percent), app::scale_metric(540, percent));
    }

    void on_row()
    {
        GtkListBoxRow* row = gtk_list_box_get_selected_row(GTK_LIST_BOX(list_));
        if (!row) return;
        render(gtk_list_box_row_get_index(row));
    }

    void render(int topic)
    {
        gtk_text_buffer_set_text(buffer_, "", 0);
        const std::vector<app::HelpBlock> blocks = app::parse_help_markup(app::help_text(static_cast<app::HelpTopic>(topic)));
        for (const app::HelpBlock& block : blocks) {
            GtkTextIter it;
            gtk_text_buffer_get_end_iter(buffer_, &it);
            const gint start = gtk_text_iter_get_offset(&it);
            std::string prefix;
            const char* tag = "para";
            switch (block.kind) {
            case app::HelpBlockKind::Heading: tag = "heading"; break;
            case app::HelpBlockKind::Subheading: tag = "sub"; break;
            case app::HelpBlockKind::Bullet: tag = "bullet"; prefix = "\xE2\x80\xA2  "; break;
            case app::HelpBlockKind::Step: tag = "step"; prefix = std::to_string(block.number) + ".  "; break;
            default: break;
            }
            gtk_text_buffer_insert(buffer_, &it, prefix.c_str(), -1);
            for (const app::HelpRun& run : block.runs) {
                gtk_text_buffer_get_end_iter(buffer_, &it);
                const gint run_start = gtk_text_iter_get_offset(&it);
                gtk_text_buffer_insert(buffer_, &it, run.text.c_str(), -1);
                if (run.bold) {
                    GtkTextIter a, b;
                    gtk_text_buffer_get_iter_at_offset(buffer_, &a, run_start);
                    gtk_text_buffer_get_end_iter(buffer_, &b);
                    gtk_text_buffer_apply_tag_by_name(buffer_, "bold", &a, &b);
                }
            }
            gtk_text_buffer_get_end_iter(buffer_, &it);
            gtk_text_buffer_insert(buffer_, &it, "\n", -1);
            GtkTextIter a, b;
            gtk_text_buffer_get_iter_at_offset(buffer_, &a, start);
            gtk_text_buffer_get_end_iter(buffer_, &b);
            gtk_text_buffer_apply_tag_by_name(buffer_, tag, &a, &b);
        }
        GtkTextIter top;
        gtk_text_buffer_get_start_iter(buffer_, &top);
        gtk_text_buffer_place_cursor(buffer_, &top);
        gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(view_), &top, 0, FALSE, 0, 0);
    }

    CompanionHost host_;
    GtkWidget* window_ = nullptr;
    GtkWidget* list_ = nullptr;
    GtkWidget* view_ = nullptr;
    GtkTextBuffer* buffer_ = nullptr;
    bool alive_ = true;
};

}   // namespace

std::unique_ptr<CompanionWindow> create_help_window(CompanionHost& host, int topic)
{
    std::unique_ptr<HelpWindow> window(new HelpWindow(host));
    window->select(topic);
    return window;
}

void help_window_select(CompanionWindow* window, int topic)
{
    if (HelpWindow* help = dynamic_cast<HelpWindow*>(window)) help->select(topic);
}

}   // namespace gtkui
}   // namespace midibn
