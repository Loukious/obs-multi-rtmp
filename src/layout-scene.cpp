#include "layout-scene.h"
#include <obs.hpp>
#include <obs-module.h>
#include <graphics/vec2.h>
#include <QDialog>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QResizeEvent>
#include <QWindow>
#include <algorithm>

bool EnsureLayoutCanvasVideo(obs_canvas_t* canvas, const LayoutVideoConfig& config, QString* error)
{
    if (!canvas || obs_canvas_removed(canvas) || (obs_canvas_get_flags(canvas) & (MAIN | EPHEMERAL))) {
        *error = obs_module_text("Primary.InvalidCanvas"); return false;
    }
    if (!obs_canvas_has_video(canvas)) {
        obs_video_info restored{};
        if (!obs_get_video_info(&restored)) { *error = "OBS video is unavailable."; return false; }
        restored.base_width = restored.output_width = config.width;
        restored.base_height = restored.output_height = config.height;
        if (!obs_canvas_reset_video(canvas, &restored)) {
            *error = "Could not restore video for the saved TikTok canvas."; return false;
        }
    }
    obs_video_info current{};
    if (!obs_canvas_get_video_info(canvas, &current)
        || current.output_width != static_cast<uint32_t>(config.width)
        || current.output_height != static_cast<uint32_t>(config.height)) {
        *error = obs_module_text("Primary.InvalidCanvas"); return false;
    }
    return true;
}

LayoutScene::~LayoutScene()
{
    if (canvas_ && !borrowedCanvas_) obs_canvas_set_channel(canvas_, 0, nullptr);
    if (wrapper_) { obs_canvas_scene_remove(wrapper_); obs_scene_release(wrapper_); }
    if (source_) obs_source_release(source_);
    if (canvas_) { if (!borrowedCanvas_) obs_canvas_remove(canvas_); obs_canvas_release(canvas_); }
}
bool LayoutScene::Create(const LayoutVideoConfig& config, QString* error)
{
    if (canvas_) { *error = "Layout scene already initialized."; return false; }
    source_ = obs_get_source_by_uuid(config.sceneUuid.c_str());
    if (!source_ || !obs_source_is_scene(source_) || obs_source_removed(source_)) {
        *error = obs_module_text("Dual.Error.SceneMissing"); return false;
    }
    if (!config.canvasUuid.empty()) {
        borrowedCanvas_ = true;
        canvas_ = obs_get_canvas_by_uuid(config.canvasUuid.c_str());
        auto owner = obs_source_get_canvas(source_);
        const bool matches = owner && owner == canvas_;
        if (owner) obs_canvas_release(owner);
        if (!matches) {
            *error = obs_module_text("Primary.InvalidCanvas"); return false;
        }
        if (!EnsureLayoutCanvasVideo(canvas_, config, error)) return false;
        obs_canvas_set_channel(canvas_, 0, source_);
        return true;
    }
    obs_video_info info{};
    if (!obs_get_video_info(&info)) { *error = "OBS video is unavailable."; return false; }
    info.base_width = info.output_width = static_cast<uint32_t>(config.width);
    info.base_height = info.output_height = static_cast<uint32_t>(config.height);
    canvas_ = obs_canvas_create_private("TikTok layout", &info, ACTIVATE | SCENE_REF | EPHEMERAL);
    if (!canvas_ || !obs_canvas_has_video(canvas_)) { *error = "Could not create the layout canvas."; return false; }
    wrapper_ = obs_canvas_scene_create(canvas_, "TikTok layout composition");
    if (!wrapper_) { *error = "Could not create the layout scene."; return false; }
    auto item = obs_scene_add(wrapper_, source_);
    if (!item) { *error = "Could not add the selected scene to the layout."; return false; }
    vec2 bounds; vec2_set(&bounds, static_cast<float>(config.width), static_cast<float>(config.height));
    obs_sceneitem_set_alignment(item, OBS_ALIGN_TOP | OBS_ALIGN_LEFT);
    obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
    obs_sceneitem_set_bounds_type(item, config.crop ? OBS_BOUNDS_SCALE_OUTER : OBS_BOUNDS_SCALE_INNER);
    obs_sceneitem_set_bounds_crop(item, config.crop);
    obs_sceneitem_set_bounds(item, &bounds);
    obs_canvas_set_channel(canvas_, 0, obs_scene_get_source(wrapper_));
    return true;
}
bool LayoutScene::CreatePreview(const LayoutVideoConfig& config, QString* error)
{
    if (canvas_) { *error = "Layout scene already initialized."; return false; }
    if (!config.canvasUuid.empty()) {
        OBSSourceAutoRelease source = obs_get_source_by_uuid(config.sceneUuid.c_str());
        if (!source || !obs_source_is_scene(source) || obs_source_removed(source)) {
            *error = obs_module_text("Dual.Error.SceneMissing"); return false;
        }
        OBSCanvasAutoRelease canvas = obs_get_canvas_by_uuid(config.canvasUuid.c_str());
        OBSCanvasAutoRelease owner = obs_source_get_canvas(source);
        if (!canvas || owner != canvas) { *error = obs_module_text("Primary.InvalidCanvas"); return false; }
        // Reloaded scenes have zero dimensions until their canvas video is
        // restored. Initialize it before nesting the scene in the preview;
        // do not switch the persistent canvas's current scene.
        if (!EnsureLayoutCanvasVideo(canvas, config, error)) return false;
    }
    auto previewConfig = config;
    previewConfig.canvasUuid.clear();
    return Create(previewConfig, error);
}
video_t* LayoutScene::Video() const { return canvas_ ? obs_canvas_get_video(canvas_) : nullptr; }
void LayoutScene::Render() const { if (canvas_) obs_canvas_render(canvas_); }
bool LayoutScene::SourceExists() const {
    if (!source_ || obs_source_removed(source_)) return false;
    auto canvas = obs_source_get_canvas(source_);
    const bool available = canvas && !obs_canvas_removed(canvas);
    if (canvas) obs_canvas_release(canvas);
    return available;
}

namespace {
class Preview : public QWidget {
public:
    LayoutScene scene;
    int canvasWidth, canvasHeight;
    obs_display_t* display = nullptr;
    Preview(QWidget* parent, int width, int height) : QWidget(parent), canvasWidth(width), canvasHeight(height) {
        setAttribute(Qt::WA_PaintOnScreen);
        setAttribute(Qt::WA_StaticContents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAttribute(Qt::WA_DontCreateNativeAncestors);
        setAttribute(Qt::WA_NativeWindow);
        setMinimumSize(width > height ? 640 : 270, width > height ? 360 : 480);
    }
    ~Preview() override { if (display) { obs_display_remove_draw_callback(display, Draw, this); obs_display_destroy(display); } }
    QPaintEngine* paintEngine() const override { return nullptr; }
    static void Draw(void* opaque, uint32_t width, uint32_t height) {
        auto& self = *static_cast<Preview*>(opaque);
        const float scale = std::min(static_cast<float>(width) / self.canvasWidth, static_cast<float>(height) / self.canvasHeight);
        const int w = static_cast<int>(self.canvasWidth * scale), h = static_cast<int>(self.canvasHeight * scale);
        gs_viewport_push(); gs_projection_push();
        gs_set_viewport((static_cast<int>(width) - w) / 2, (static_cast<int>(height) - h) / 2, w, h);
        gs_ortho(0, static_cast<float>(self.canvasWidth), 0, static_cast<float>(self.canvasHeight), -100, 100);
        self.scene.Render();
        gs_projection_pop(); gs_viewport_pop();
    }
    void showEvent(QShowEvent* event) override {
        QWidget::showEvent(event);
        CreateDisplay();
    }
    void paintEvent(QPaintEvent*) override { CreateDisplay(); }
    void CreateDisplay() {
        if (display || !windowHandle() || !windowHandle()->isExposed()) return;
        gs_init_data data{};
#ifdef _WIN32
        data.window.hwnd = reinterpret_cast<void*>(winId());
#endif
        data.cx = static_cast<uint32_t>(width() * devicePixelRatioF());
        data.cy = static_cast<uint32_t>(height() * devicePixelRatioF());
        data.format = GS_BGRA; data.zsformat = GS_ZS_NONE;
        display = obs_display_create(&data, 0);
        if (display) obs_display_add_draw_callback(display, Draw, this);
    }
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        if (display) obs_display_resize(display, static_cast<uint32_t>(width()*devicePixelRatioF()), static_cast<uint32_t>(height()*devicePixelRatioF()));
    }
};
}
void ShowLayoutPreview(const LayoutVideoConfig& config, QWidget* parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(obs_module_text("Dual.Preview"));
    auto layout = new QVBoxLayout(&dialog);
    auto preview = new Preview(&dialog, config.width, config.height);
    layout->addWidget(preview);
    QString error;
    if (!preview->scene.CreatePreview(config, &error)) { QMessageBox::warning(parent, "TikTok", error); return; }
    dialog.exec();
}
