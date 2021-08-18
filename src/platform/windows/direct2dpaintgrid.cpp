#include "wineditor.hpp"
#include "direct2dpaintgrid.hpp"
#include <d2d1.h>
#include <ranges>

void D2DPaintGrid::set_size(u16 w, u16 h)
{
  GridBase::set_size(w, h);
  update_bitmap_size();
}

void D2DPaintGrid::update_bitmap_size()
{
  auto&& [font_width, font_height] = editor_area->font_dimensions();
  u32 width = std::ceil(cols * font_width);
  u32 height = std::ceil(rows * font_height);
  editor_area->resize_bitmap(context, &bitmap, width, height);
  context->SetTarget(bitmap);
  context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
}

void D2DPaintGrid::initialize_context()
{
  editor_area->create_context(&context, &bitmap, 0, 0);
  context->SetTarget(bitmap);
}

void D2DPaintGrid::process_events()
{
  context->BeginDraw();
  ID2D1SolidColorBrush* fg_brush = nullptr;
  ID2D1SolidColorBrush* bg_brush = nullptr;
  u32 fg = editor_area->default_fg().rgb();
  u32 bg = editor_area->default_bg().rgb();
  context->CreateSolidColorBrush(d2color(fg), &fg_brush);
  context->CreateSolidColorBrush(d2color(bg), &bg_brush);
  auto&& [font_width, font_height] = editor_area->font_dimensions();
  while(!evt_q.empty())
  {
    const auto& evt = evt_q.front();
    switch(evt.type)
    {
      case PaintKind::Clear:
      {
        d2rect r = {0, 0, cols * font_width, rows * font_height};
        bg_brush->SetColor(d2color(bg));
        context->FillRectangle(r, bg_brush);
        break;
      }
      case PaintKind::Redraw:
        draw(context, {0, 0, cols, rows}, fg_brush, bg_brush);
        clear_event_queue();
        break;
      case PaintKind::Draw:
        draw(context, evt.rect, fg_brush, bg_brush);
        break;
    }
    if (!evt_q.empty()) evt_q.pop();
  }
  SafeRelease(&fg_brush);
  SafeRelease(&bg_brush);
  context->EndDraw();
}

void D2DPaintGrid::draw_text_and_bg(
  ID2D1RenderTarget* context,
  const QString& buf,
  const HLAttr& attr,
  const HLAttr& fallback,
  D2D1_POINT_2F start,
  D2D1_POINT_2F end,
  IDWriteTextFormat* text_format,
  ID2D1SolidColorBrush* fg_brush,
  ID2D1SolidColorBrush* bg_brush
)
{
  HRESULT hr;
  auto* factory = editor_area->dwrite_factory();
  IDWriteTextLayout* old_text_layout = nullptr;
  hr = factory->CreateTextLayout(
    (LPCWSTR) buf.utf16(),
    buf.size(),
    text_format,
    end.x - start.x,
    end.y - start.y,
    &old_text_layout
  );
  if (FAILED(hr)) return;
  // IDWriteTextLayout1 can set char spacing
  IDWriteTextLayout1* text_layout = nullptr;
  hr = old_text_layout->QueryInterface(&text_layout);
  if (FAILED(hr)) return;
  DWRITE_TEXT_RANGE text_range {0, (UINT32) buf.size()};
  auto charspace = editor_area->charspacing();
  if (charspace)
  {
    text_layout->SetCharacterSpacing(0, float(charspace), 0, text_range);
  }
  if (attr.italic())
  {
    text_layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, text_range);
  }
  if (attr.bold())
  {
    text_layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, text_range);
  }
  if (attr.underline())
  {
    text_layout->SetUnderline(true, text_range);
  }
  auto fg = attr.fg().value_or(*fallback.fg()).to_uint32();
  auto bg = attr.bg().value_or(*fallback.bg()).to_uint32();
  if (attr.reverse) std::swap(fg, bg);
  D2D1_RECT_F bg_rect = {start.x, start.y, end.x, end.y};
  fg_brush->SetColor(D2D1::ColorF(fg));
  bg_brush->SetColor(D2D1::ColorF(bg));
  context->FillRectangle(bg_rect, bg_brush);
  auto offset = float(editor_area->linespacing()) / 2.f;
  D2D1_POINT_2F text_pt = {start.x, start.y + offset};
  context->DrawTextLayout(
    text_pt,
    text_layout,
    fg_brush,
    D2D1_DRAW_TEXT_OPTIONS_NONE
  );
  SafeRelease(&old_text_layout);
  SafeRelease(&text_layout);
}

void D2DPaintGrid::draw(
  ID2D1RenderTarget* context,
  QRect r,
  ID2D1SolidColorBrush* fg_brush,
  ID2D1SolidColorBrush* bg_brush
)
{
  const auto& fonts = editor_area->fallback_list();
  const int start_x = r.left(), end_x = r.right();
  const int start_y = r.top(), end_y = r.bottom();
  const auto font_dims = editor_area->font_dimensions();
  const float font_width = std::get<0>(font_dims);
  const float font_height = std::get<1>(font_dims);
  QString buffer;
  buffer.reserve(100);
  const HLState* s = editor_area->hl_state();
  const auto& def_clrs = s->default_colors_get();
  u32 cur_font_idx = 0;
  const auto get_pos = [&](int x, int y, int num_chars) {
    d2pt tl = {x * font_width, y * font_height};
    d2pt br = {(x + num_chars) * font_width, (y + 1) * font_height};
    return std::tuple {tl, br};
  };
  const auto draw_buf = [&](const HLAttr& main, d2pt start, d2pt end) {
    if (buffer.isEmpty()) return;
    const auto& tf = fonts[cur_font_idx];
    draw_text_and_bg(
      context, buffer, main, def_clrs, start, end,
      tf, fg_brush, bg_brush
    );
    buffer.clear();
  };
  for(int y = start_y; y <= end_y && y < rows; ++y)
  {
    d2pt start = {0, y * font_height};
    std::uint16_t prev_hl_id = UINT16_MAX;
    for(int x = 0; x < cols; ++x)
    {
      const auto& gc = area[y * cols + x];
      const auto font_idx = editor_area->font_for_ucs(gc.ucs);
      if (font_idx != cur_font_idx
          && !(gc.text.isEmpty() || gc.text.at(0).isSpace()))
      {
        auto&& [tl, br] = get_pos(x, y, 0);
        draw_buf(s->attr_for_id(prev_hl_id), start, br);
        start = tl;
        cur_font_idx = font_idx;
      }
      if (gc.double_width)
      {
        auto&& [tl, br] = get_pos(x, y, 2);
        d2pt buf_end = {tl.x, tl.y + font_height};
        draw_buf(s->attr_for_id(prev_hl_id), start, buf_end);
        buffer.append(gc.text);
        draw_buf(s->attr_for_id(gc.hl_id), tl, br);
        start = {br.x, br.y - font_height};
        prev_hl_id = gc.hl_id;
      }
      else if (gc.hl_id == prev_hl_id)
      {
        buffer.append(gc.text);
        continue;
      }
      else
      {
        auto&& [tl, br] = get_pos(x, y, 1);
        draw_buf(s->attr_for_id(prev_hl_id), start, br);
        start = tl;
        prev_hl_id = gc.hl_id;
        buffer.append(gc.text);
      }
    }
    d2pt br = {cols * font_width, (y + 1) * font_height};
    draw_buf(s->attr_for_id(prev_hl_id), start, br);
  }
}

D2DPaintGrid::d2pt D2DPaintGrid::pos() const
{
  auto&& [font_width, font_height] = editor_area->font_dimensions();
  return {x * font_width, y * font_height};
}

D2DPaintGrid::d2rect D2DPaintGrid::rect() const
{
  auto size = context->GetSize();
  float left = top_left.x();
  float top = top_left.y();
  float right = left + size.width;
  float bottom = top + size.height;
  return {left, top, right, bottom};
}

D2DPaintGrid::d2rect D2DPaintGrid::source_rect() const
{
  auto size = context->GetSize();
  return D2D1::RectF(0, 0, size.width, size.height);
}

void D2DPaintGrid::set_pos(u16 new_x, u16 new_y)
{
  // If x_diff is negative, then new_x is to the left of x.
  auto x_diff = new_x - x;
  // If y_diff is negative, then new_y is above y.
  auto y_diff = new_y - y;
  auto old_x = x, old_y = y;
  if (!editor_area->animations_enabled())
  {
    GridBase::set_pos(new_x, new_y);
    update_position(new_x, new_y);
    return;
  }
  move_animation_time = editor_area->move_animation_duration();
  auto interval = editor_area->move_animation_frametime();
  move_update_timer.disconnect();
  move_update_timer.setInterval(interval);
  move_update_timer.callOnTimeout([=] {
    auto ms_interval = move_update_timer.interval();
    move_animation_time -= float(ms_interval) / 1000.f;
    if (move_animation_time <= 0)
    {
      move_update_timer.stop();
      update_position(new_x, new_y);
    }
    else
    {
      auto duration = editor_area->move_animation_duration();
      // What % of the animation is left (between 0 and 1)
      auto animation_left = move_animation_time / duration;
      float animation_finished = 1.0f - animation_left;
      float animated_x = old_x + (float(x_diff) * animation_finished);
      float animated_y = old_y + (float(y_diff) * animation_finished);
      update_position(animated_x, animated_y);
    }
    editor_area->update();
  });
  GridBase::set_pos(new_x, new_y);
  move_update_timer.start();
}

void D2DPaintGrid::update_position(double x, double y)
{
  auto&& [font_width, font_height] = editor_area->font_dimensions();
  top_left = {x * font_width, y * font_height};
}

void D2DPaintGrid::viewport_changed(Viewport vp)
{
  // Thanks to Keith Simmons, the Neovide developer for explaining
  // his implementation of Neovide's smooth scrolling in his blog post
  // at http://02credits.com/blog/day96-neovide-smooth-scrolling.
  if (!editor_area->animations_enabled() || viewport.topline == vp.topline)
  {
    GridBase::viewport_changed(vp);
    return;
  }
  auto dest_topline = vp.topline;
  start_scroll_y = current_scroll_y;
  auto diff = float(dest_topline) - start_scroll_y;
  snapshots.push_back({vp, copy_bitmap(bitmap)});
  if (snapshots.size() > 2)
  {
    SafeRelease(&snapshots[0].image);
    snapshots.erase(snapshots.begin());
  }
  GridBase::viewport_changed(vp);
  scroll_animation_timer.disconnect();
  auto interval = editor_area->scroll_animation_frametime();
  scroll_animation_time = editor_area->scroll_animation_duration();
  scroll_animation_timer.setInterval(interval);
  scroll_animation_timer.callOnTimeout([=] {
    auto timer_interval = scroll_animation_timer.interval();
    scroll_animation_time -= float(timer_interval) / 1000.f;
    if (scroll_animation_time <= 0.f)
    {
      scroll_animation_timer.stop();
      is_scrolling = false;
    }
    else
    {
      auto duration = editor_area->scroll_animation_duration();
      auto animation_left = scroll_animation_time / duration;
      float animation_finished = 1.0f - animation_left;
      float scaled = 1.0f - std::pow(2.0, animation_finished * -10.0f);
      current_scroll_y = start_scroll_y + (diff * scaled);
    }
    editor_area->update();
  });
  is_scrolling = true;
  scroll_animation_timer.start();
}

ID2D1Bitmap1* D2DPaintGrid::copy_bitmap(ID2D1Bitmap1* src)
{
  assert(src);
  ID2D1Bitmap1* dst = nullptr;
  auto sz = src->GetPixelSize();
  editor_area->resize_bitmap(context, &dst, sz.width, sz.height);
  auto tl = D2D1::Point2U(0, 0);
  auto src_rect = D2D1::RectU(0, 0, sz.width, sz.height);
  dst->CopyFromBitmap(&tl, src, &src_rect);
  return dst;
}

void D2DPaintGrid::render(ID2D1RenderTarget* render_target)
{
  auto&& [font_width, font_height] = editor_area->font_dimensions();
  auto sz = bitmap->GetPixelSize();
  d2rect r = rect();
  if (!editor_area->animations_enabled() || !is_scrolling)
  {
    render_target->DrawBitmap(
      bitmap,
      &r,
      1.0f,
      D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
    );
    return;
  }
  render_target->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
  auto bg = editor_area->default_bg().rgb();
  ID2D1SolidColorBrush* bg_brush = nullptr;
  render_target->CreateSolidColorBrush(D2D1::ColorF(bg), &bg_brush);
  render_target->FillRectangle(r, bg_brush);
  SafeRelease(&bg_brush);
  float cur_scroll_y = current_scroll_y * font_height;
  float cur_snapshot_top = viewport.topline * font_height;
  for(auto& snapshot : snapshots | std::views::reverse)
  {
    float snapshot_top = snapshot.vp.topline * font_height;
    float offset = snapshot_top - cur_scroll_y;
    auto pixmap_top = top_left.y() + offset - font_height;
    d2pt pt = D2D1::Point2F(top_left.x(), pixmap_top);
    r = D2D1::RectF(pt.x, pt.y, pt.x + sz.width, pt.y + sz.height);
    render_target->DrawBitmap(
      snapshot.image,
      &r,
      1.0f,
      D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
    );
  }
  float offset = cur_snapshot_top - cur_scroll_y;
  d2pt pt = D2D1::Point2F(top_left.x(), top_left.y() + offset);
  r = D2D1::RectF(pt.x, pt.y, pt.x + sz.width, pt.y + sz.height);
  render_target->DrawBitmap(
    bitmap,
    &r,
    1.0f,
    D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
  );
  render_target->PopAxisAlignedClip();
}

D2DPaintGrid::~D2DPaintGrid()
{
  for(auto& snapshot : snapshots) SafeRelease(&snapshot.image);
  SafeRelease(&bitmap);
  SafeRelease(&context);
}
