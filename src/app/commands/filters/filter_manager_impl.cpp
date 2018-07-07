// Aseprite
// Copyright (C) 2001-2018  David Capello
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "app/commands/filters/filter_manager_impl.h"

#include "app/app.h"
#include "app/cmd/copy_region.h"
#include "app/cmd/patch_cel.h"
#include "app/cmd/set_palette.h"
#include "app/cmd/unlink_cel.h"
#include "app/context_access.h"
#include "app/doc.h"
#include "app/ini_file.h"
#include "app/modules/editors.h"
#include "app/modules/palettes.h"
#include "app/site.h"
#include "app/transaction.h"
#include "app/ui/color_bar.h"
#include "app/ui/editor/editor.h"
#include "app/ui/palette_view.h"
#include "app/ui/timeline/timeline.h"
#include "app/util/range_utils.h"
#include "doc/algorithm/shrink_bounds.h"
#include "doc/cel.h"
#include "doc/cels_range.h"
#include "doc/image.h"
#include "doc/layer.h"
#include "doc/mask.h"
#include "doc/sprite.h"
#include "filters/filter.h"
#include "ui/manager.h"
#include "ui/view.h"
#include "ui/widget.h"

#include <cstdlib>
#include <cstring>
#include <set>

namespace app {

using namespace std;
using namespace ui;

FilterManagerImpl::FilterManagerImpl(Context* context, Filter* filter)
  : m_context(context)
  , m_site(context->activeSite())
  , m_filter(filter)
  , m_cel(nullptr)
  , m_src(nullptr)
  , m_dst(nullptr)
  , m_row(0)
  , m_mask(nullptr)
  , m_previewMask(nullptr)
  , m_targetOrig(TARGET_ALL_CHANNELS)
  , m_target(TARGET_ALL_CHANNELS)
  , m_celsTarget(CelsTarget::Selected)
  , m_oldPalette(nullptr)
  , m_progressDelegate(NULL)
{
  int x, y;
  Image* image = m_site.image(&x, &y);
  if (!image
      || (m_site.layer() &&
          m_site.layer()->isReference()))
    throw NoImageException();

  init(m_site.cel());
}

FilterManagerImpl::~FilterManagerImpl()
{
  if (m_oldPalette) {
    restoreSpritePalette();
    set_current_palette(m_oldPalette.get(), false);
  }
}

Doc* FilterManagerImpl::document()
{
  return static_cast<Doc*>(m_site.document());
}

void FilterManagerImpl::setProgressDelegate(IProgressDelegate* progressDelegate)
{
  m_progressDelegate = progressDelegate;
}

PixelFormat FilterManagerImpl::pixelFormat() const
{
  return m_site.sprite()->pixelFormat();
}

void FilterManagerImpl::setTarget(int target)
{
  m_targetOrig = target;
  m_target = target;

  // The alpha channel of the background layer can't be modified.
  if (m_site.layer() &&
      m_site.layer()->isBackground())
    m_target &= ~TARGET_ALPHA_CHANNEL;
}

void FilterManagerImpl::setCelsTarget(CelsTarget celsTarget)
{
  m_celsTarget = celsTarget;
}

void FilterManagerImpl::begin()
{
  Doc* document = m_site.document();

  m_row = 0;
  m_mask = (document->isMaskVisible() ? document->mask(): nullptr);
  updateBounds(m_mask);
}

void FilterManagerImpl::beginForPreview()
{
  Doc* document = m_site.document();

  if (document->isMaskVisible())
    m_previewMask.reset(new Mask(*document->mask()));
  else {
    m_previewMask.reset(new Mask());
    m_previewMask->replace(m_site.sprite()->bounds());
  }

  m_row = m_nextRowToFlush = 0;
  m_mask = m_previewMask;

  Editor* editor = current_editor;
  // If we have a tiled mode enabled, we'll apply the filter to the whole areaes
  if (editor->docPref().tiled.mode() == filters::TiledMode::NONE) {
    Sprite* sprite = m_site.sprite();
    gfx::Rect vp = View::getView(editor)->viewportBounds();
    vp = editor->screenToEditor(vp);
    vp = vp.createIntersection(sprite->bounds());

    if (vp.isEmpty()) {
      m_previewMask.reset(nullptr);
      m_row = -1;
      return;
    }

    m_previewMask->intersect(vp);
  }

  if (!updateBounds(m_mask)) {
    m_previewMask.reset(nullptr);
    m_row = -1;
    return;
  }
}

void FilterManagerImpl::end()
{
  m_maskBits.unlock();
}

bool FilterManagerImpl::applyStep()
{
  if (m_row < 0 || m_row >= m_bounds.h)
    return false;

  if (m_mask && m_mask->bitmap()) {
    int x = m_bounds.x - m_mask->bounds().x;
    int y = m_bounds.y - m_mask->bounds().y + m_row;
    if ((x >= m_bounds.w) ||
        (y >= m_bounds.h))
      return false;

    m_maskBits = m_mask->bitmap()
      ->lockBits<BitmapTraits>(Image::ReadLock,
        gfx::Rect(x, y, m_bounds.w - x, m_bounds.h - y));

    m_maskIterator = m_maskBits.begin();
  }

  switch (m_site.sprite()->pixelFormat()) {
    case IMAGE_RGB:       m_filter->applyToRgba(this); break;
    case IMAGE_GRAYSCALE: m_filter->applyToGrayscale(this); break;
    case IMAGE_INDEXED:   m_filter->applyToIndexed(this); break;
  }
  ++m_row;

  return true;
}

void FilterManagerImpl::apply()
{
  bool cancelled = false;

  begin();
  while (!cancelled && applyStep()) {
    if (m_progressDelegate) {
      // Report progress.
      m_progressDelegate->reportProgress(m_progressBase + m_progressWidth * (m_row+1) / m_bounds.h);

      // Does the user cancelled the whole process?
      cancelled = m_progressDelegate->isCancelled();
    }
  }

  if (!cancelled) {
    gfx::Rect output;
    if (algorithm::shrink_bounds2(m_src.get(), m_dst.get(),
                                  m_bounds, output)) {
      if (m_cel->layer()->isBackground()) {
        m_transaction->execute(
          new cmd::CopyRegion(
            m_cel->image(),
            m_dst.get(),
            gfx::Region(output),
            position()));
      }
      else {
        // Patch "m_cel"
        m_transaction->execute(
          new cmd::PatchCel(
            m_cel, m_dst.get(),
            gfx::Region(output),
            position()));
      }
    }
  }
}

void FilterManagerImpl::applyToTarget()
{
  const bool paletteChange = paletteHasChanged();
  bool cancelled = false;

  CelList cels;

  switch (m_celsTarget) {

    case CelsTarget::Selected: {
      auto range = App::instance()->timeline()->range();
      if (range.enabled()) {
        for (Cel* cel : get_unlocked_unique_cels(m_site.sprite(), range)) {
          if (!cel->layer()->isReference())
            cels.push_back(cel);
        }
      }
      else if (m_site.cel() &&
               m_site.layer() &&
               m_site.layer()->isEditable() &&
               !m_site.layer()->isReference()) {
        cels.push_back(m_site.cel());
      }
      break;
    }

    case CelsTarget::All: {
      for (Cel* cel : m_site.sprite()->uniqueCels()) {
        if (cel->layer()->isEditable() &&
            !cel->layer()->isReference())
          cels.push_back(cel);
      }
      break;
    }
  }

  if (cels.empty() && !paletteChange) {
    // We don't have images/palette changes to do (there will not be a
    // transaction).
    return;
  }

  // Initialize writting operation
  ContextReader reader(m_context);
  ContextWriter writer(reader);
  m_transaction.reset(new Transaction(writer.context(), m_filter->getName(), ModifyDocument));

  m_progressBase = 0.0f;
  m_progressWidth = (cels.size() > 0 ? 1.0f / cels.size(): 1.0f);

  std::set<ObjectId> visited;

  // Palette change
  if (paletteChange) {
    Palette newPalette = *getNewPalette();
    restoreSpritePalette();
    m_transaction->execute(
      new cmd::SetPalette(m_site.sprite(),
                          m_site.frame(), &newPalette));
  }

  // For each target image
  for (auto it = cels.begin();
       it != cels.end() && !cancelled;
       ++it) {
    Image* image = (*it)->image();

    // Avoid applying the filter two times to the same image
    if (visited.find(image->id()) == visited.end()) {
      visited.insert(image->id());
      applyToCel(*it);
    }

    // Is there a delegate to know if the process was cancelled by the user?
    if (m_progressDelegate)
      cancelled = m_progressDelegate->isCancelled();

    // Make progress
    m_progressBase += m_progressWidth;
  }

  // Reset m_oldPalette to avoid restoring the color palette
  m_oldPalette.reset(nullptr);
}

bool FilterManagerImpl::isTransaction() const
{
  return m_transaction != nullptr;
}

// This must be executed in the main UI thread.
// Check Transaction::commit() comments.
void FilterManagerImpl::commitTransaction()
{
  ASSERT(m_transaction);
  m_transaction->commit();
}

void FilterManagerImpl::flush()
{
  int h = m_row - m_nextRowToFlush;

  if (m_row >= 0 && h > 0) {
    Editor* editor = current_editor;

    // Redraw the color palette
    if (m_nextRowToFlush == 0 && paletteHasChanged())
      redrawColorPalette();

    // We expand the region one pixel at the top and bottom of the
    // region [m_row,m_nextRowToFlush) to be updated on the screen to
    // avoid screen artifacts when we apply filters like convolution
    // matrices.
    gfx::Rect rect(
      editor->editorToScreen(
        gfx::Point(
          m_bounds.x,
          m_bounds.y+m_nextRowToFlush-1)),
      gfx::Size(
        editor->projection().applyX(m_bounds.w),
        (editor->projection().scaleY() >= 1 ? editor->projection().applyY(h+2):
                                              editor->projection().removeY(h+2))));

    gfx::Region reg1(rect);
    editor->expandRegionByTiledMode(reg1, true);

    gfx::Region reg2;
    editor->getDrawableRegion(reg2, Widget::kCutTopWindows);
    reg1.createIntersection(reg1, reg2);

    editor->invalidateRegion(reg1);
    m_nextRowToFlush = m_row+1;
  }
}

void FilterManagerImpl::disablePreview()
{
  current_editor->invalidate();

  // Redraw the color bar in case the filter modified the palette.
  if (paletteHasChanged()) {
    restoreSpritePalette();
    redrawColorPalette();
  }
}

const void* FilterManagerImpl::getSourceAddress()
{
  return m_src->getPixelAddress(m_bounds.x, m_bounds.y+m_row);
}

void* FilterManagerImpl::getDestinationAddress()
{
  return m_dst->getPixelAddress(m_bounds.x, m_bounds.y+m_row);
}

bool FilterManagerImpl::skipPixel()
{
  bool skip = false;

  if ((m_mask) && (m_mask->bitmap())) {
    if (!*m_maskIterator)
      skip = true;

    ++m_maskIterator;
  }

  return skip;
}

const Palette* FilterManagerImpl::getPalette() const
{
  if (m_oldPalette)
    return m_oldPalette.get();
  else
    return m_site.sprite()->palette(m_site.frame());
}

const RgbMap* FilterManagerImpl::getRgbMap() const
{
  return m_site.sprite()->rgbMap(m_site.frame());
}

Palette* FilterManagerImpl::getNewPalette()
{
  if (!m_oldPalette)
    m_oldPalette.reset(new Palette(*getPalette()));
  return m_site.sprite()->palette(m_site.frame());
}

doc::PalettePicks FilterManagerImpl::getPalettePicks()
{
  doc::PalettePicks picks;
  ColorBar::instance()
    ->getPaletteView()
    ->getSelectedEntries(picks);
  return picks;
}

void FilterManagerImpl::init(Cel* cel)
{
  ASSERT(cel);

  Doc* doc = m_site.document();
  if (!updateBounds(doc->isMaskVisible() ? doc->mask(): nullptr))
    throw InvalidAreaException();

  m_cel = cel;
  m_src.reset(
    crop_image(
      cel->image(),
      gfx::Rect(m_site.sprite()->bounds()).offset(-cel->position()), 0));
  m_dst.reset(Image::createCopy(m_src.get()));

  m_row = -1;
  m_mask = nullptr;
  m_previewMask.reset(nullptr);

  m_target = m_targetOrig;

  // The alpha channel of the background layer can't be modified
  if (cel->layer()->isBackground())
    m_target &= ~TARGET_ALPHA_CHANNEL;
}

void FilterManagerImpl::applyToCel(Cel* cel)
{
  init(cel);
  apply();
}

bool FilterManagerImpl::updateBounds(doc::Mask* mask)
{
  gfx::Rect bounds;
  if (mask && mask->bitmap() && !mask->bounds().isEmpty()) {
    bounds = mask->bounds();
    bounds &= m_site.sprite()->bounds();
  }
  else {
    bounds = m_site.sprite()->bounds();
  }
  m_bounds = bounds;
  return !m_bounds.isEmpty();
}

bool FilterManagerImpl::paletteHasChanged()
{
  return
    (m_oldPalette &&
     getPalette()->countDiff(getNewPalette(), nullptr, nullptr));
}

void FilterManagerImpl::restoreSpritePalette()
{
  // Restore the original palette to save the undoable "cmd"
  if (m_oldPalette)
    m_site.sprite()->setPalette(m_oldPalette.get(), false);
}

void FilterManagerImpl::redrawColorPalette()
{
  set_current_palette(getNewPalette(), false);
  ColorBar::instance()->invalidate();
}

bool FilterManagerImpl::isMaskActive() const
{
  return m_site.document()->isMaskVisible();
}

} // namespace app
