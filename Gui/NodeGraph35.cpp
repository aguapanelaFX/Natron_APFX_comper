/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "NodeGraph.h"
#include "NodeGraphPrivate.h"

#include <cmath>
#include <stdexcept>
#include <climits>

GCC_DIAG_UNUSED_PRIVATE_FIELD_OFF
CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QAction>
#include <QFileSystemModel>
#include <QMenu>
GCC_DIAG_UNUSED_PRIVATE_FIELD_ON
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/KnobSerialization.h" // createDefaultValueForParam
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"
#include "Engine/FileSystemModel.h"

#include "Gui/ActionShortcuts.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/Menu.h"
#include "Gui/NodeGui.h"
#include <algorithm>
#include <map>
#include "Engine/SolvfxStamp.h"
#include "Engine/CreateNodeArgs.h"
#include <QCursor>
#include "Gui/SequenceFileDialog.h"
#include "Gui/ToolButton.h"

#include "Global/QtCompat.h"

NATRON_NAMESPACE_ENTER
QImage
NodeGraph::getFullSceneScreenShot()
{
    _imp->isDoingPreviewRender = true;

    // The bbox of all nodes in the nodegraph
    QRectF sceneR = _imp->calcNodesBoundingRect();

    // The visible portion of the nodegraph
    QRectF viewRect = visibleSceneRect();

    // Make sure the visible rect is included in the scene rect
    sceneR = sceneR.united(viewRect);

    int navWidth = std::ceil(width() * NATRON_NAVIGATOR_BASE_WIDTH);
    int navHeight = std::ceil(height() * NATRON_NAVIGATOR_BASE_HEIGHT);

    // Make sceneR and viewRect keep the same aspect ratio as the navigator
    double xScale = navWidth / sceneR.width();
    double yScale =  navHeight / sceneR.height();
    double scaleFactor = std::max( 0.001, std::min(xScale, yScale) );
    int sceneW_navPixelCoord = std::floor(sceneR.width() * scaleFactor);
    int sceneH_navPixelCoord = std::floor(sceneR.height() * scaleFactor);

    // Render the scene in an image with the same aspect ratio  as the scene rect
    QImage renderImage(sceneW_navPixelCoord, sceneH_navPixelCoord, QImage::Format_ARGB32_Premultiplied);

    // Fill the background
    renderImage.fill( QColor(71, 71, 71, 255) );

    // Offset the visible rect corner as an offset relative to the scene rect corner
    viewRect.setX( viewRect.x() - sceneR.x() );
    viewRect.setY( viewRect.y() - sceneR.y() );
    viewRect.setWidth( viewRect.width() - sceneR.x() );
    viewRect.setHeight( viewRect.height() - sceneR.y() );

    QRectF viewRect_navCoordinates = viewRect;
    viewRect_navCoordinates.setLeft(viewRect.left() * scaleFactor);
    viewRect_navCoordinates.setBottom(viewRect.bottom() * scaleFactor);
    viewRect_navCoordinates.setRight(viewRect.right() * scaleFactor);
    viewRect_navCoordinates.setTop(viewRect.top() * scaleFactor);

    // Paint the visible portion with a highlight
    QPainter painter(&renderImage);

    // Remove the overlays from the scene before rendering it
    scene()->removeItem(_imp->_cacheSizeText);
    scene()->removeItem(_imp->_navigator);

    // Render into the QImage with downscaling
    scene()->render(&painter, renderImage.rect(), sceneR, Qt::KeepAspectRatio);

    // Add the overlays back
    scene()->addItem(_imp->_navigator);
    scene()->addItem(_imp->_cacheSizeText);

    // Fill the highlight with a semi transparent whitish grey
    painter.fillRect( viewRect_navCoordinates, QColor(200, 200, 200, 100) );

    // Draw a border surrounding the
    QPen p;
    p.setWidth(2);
    p.setBrush(Qt::yellow);
    painter.setPen(p);
    // Make sure the border is visible
    viewRect_navCoordinates.adjust(2, 2, -2, -2);
    painter.drawRect(viewRect_navCoordinates);

    // Now make an image of the requested size of the navigator and center the render image into it
    QImage img(navWidth, navHeight, QImage::Format_ARGB32_Premultiplied);
    img.fill( QColor(71, 71, 71, 255) );

    int xOffset = ( img.width() - renderImage.width() ) / 2;
    int yOffset = ( img.height() - renderImage.height() ) / 2;
    int dstY = yOffset;
    for (int srcY = 0; srcY < renderImage.height(); ++srcY, ++dstY) {
        if ( dstY < 0 || dstY >= img.height() ) {
            break;
        }
        QRgb* dst_pixels = (QRgb*)img.scanLine(dstY);
        assert(dst_pixels);

        const QRgb* src_pixels = (const QRgb*)renderImage.scanLine(srcY);
        assert(src_pixels);

        int dstX = xOffset;
        for (int srcX = 0; srcX < renderImage.width(); ++srcX, ++dstX) {
            if ( dstX < 0 || dstX >= img.width() ) {
                dst_pixels[dstX] = qRgba(0, 0, 0, 0);
            } else {
                dst_pixels[dstX] = src_pixels[srcX];
            }
        }
    }

    _imp->isDoingPreviewRender = false;

    return img;
} // getFullSceneScreenShot

const NodesGuiList &
NodeGraph::getAllActiveNodes() const
{
    return _imp->_nodes;
}

NodesGuiList
NodeGraph::getAllActiveNodes_mt_safe() const
{
    QMutexLocker l(&_imp->_nodesMutex);

    return _imp->_nodes;
}

void
NodeGraph::moveToTrash(NodeGui* node)
{
    assert(node);
    for (NodesGuiList::iterator it = _imp->_selection.begin();
         it != _imp->_selection.end(); ++it) {
        if (it->get() == node) {
            (*it)->setUserSelected(false);
            _imp->_selection.erase(it);
            break;
        }
    }

    QMutexLocker l(&_imp->_nodesMutex);
    for (NodesGuiList::iterator it = _imp->_nodes.begin(); it != _imp->_nodes.end(); ++it) {
        if ( (*it).get() == node ) {
            _imp->_nodesTrash.push_back(*it);
            _imp->_nodes.erase(it);
            break;
        }
    }
}

void
NodeGraph::restoreFromTrash(NodeGui* node)
{
    assert(node);
    QMutexLocker l(&_imp->_nodesMutex);
    for (NodesGuiList::iterator it = _imp->_nodesTrash.begin(); it != _imp->_nodesTrash.end(); ++it) {
        if ( (*it).get() == node ) {
            _imp->_nodes.push_back(*it);
            _imp->_nodesTrash.erase(it);
            break;
        }
    }
}

// grabbed from QDirModelPrivate::size() in qtbase/src/widgets/itemviews/qdirmodel.cpp
static
QString
QDirModelPrivate_size(quint64 bytes)
{
    // According to the Si standard KB is 1000 bytes, KiB is 1024
    // but on windows sizes are calculated by dividing by 1024 so we do what they do.
    const quint64 kb = 1024;
    const quint64 mb = 1024 * kb;
    const quint64 gb = 1024 * mb;
    const quint64 tb = 1024 * gb;

    if (bytes >= tb) {
        return QFileSystemModel::tr("%1 TB").arg( QLocale().toString(qreal(bytes) / tb, 'f', 3) );
    }
    if (bytes >= gb) {
        return QFileSystemModel::tr("%1 GB").arg( QLocale().toString(qreal(bytes) / gb, 'f', 2) );
    }
    if (bytes >= mb) {
        return QFileSystemModel::tr("%1 MB").arg( QLocale().toString(qreal(bytes) / mb, 'f', 1) );
    }
    if (bytes >= kb) {
        return QFileSystemModel::tr("%1 KB").arg( QLocale().toString(bytes / kb) );
    }

    return QFileSystemModel::tr("%1 byte(s)").arg( QLocale().toString(bytes) );
}

void
NodeGraph::updateCacheSizeText()
{
    if ( !getGui() ) {
        return;
    }
    if ( getGui()->isGUIFrozen() ) {
        if ( _imp->_cacheSizeText->isVisible() ) {
            _imp->_cacheSizeText->hide();
        }

        return;
    } else {
        if ( !_imp->cacheSizeHidden && !_imp->_cacheSizeText->isVisible() ) {
            _imp->_cacheSizeText->show();
        } else if ( _imp->cacheSizeHidden && _imp->_cacheSizeText->isVisible() ) {
            _imp->_cacheSizeText->hide();

            return;
        }
    }

    QString oldText = _imp->_cacheSizeText->text();
    quint64 cacheSize = appPTR->getCachesTotalMemorySize();
    QString cacheSizeStr = QDirModelPrivate_size(cacheSize);
    quint64 diskSize = appPTR->getCachesTotalDiskSize();
    QString diskCacheSizeStr = QDirModelPrivate_size(diskSize);
    QString newText = tr("Memory cache: %1 / Disk cache: %2").arg(cacheSizeStr).arg(diskCacheSizeStr);
    if (newText != oldText) {
        _imp->_cacheSizeText->setText(newText);
    }
}

void
NodeGraph::onRefreshNodesRenderStateTimerTimeout()
{
    for (NodesGuiList::const_iterator it = _imp->_nodes.begin(); it != _imp->_nodes.end(); ++it) {
        (*it)->refreshRenderingIndicator();
    }
}

void
NodeGraph::toggleCacheInfo()
{
    _imp->cacheSizeHidden = !_imp->cacheSizeHidden;
    if (_imp->cacheSizeHidden) {
        _imp->_cacheSizeText->hide();
    } else {
        _imp->_cacheSizeText->show();
    }
}

void
NodeGraph::toggleKnobLinksVisible()
{
    _imp->_knobLinksVisible = !_imp->_knobLinksVisible;
    {
        QMutexLocker l(&_imp->_nodesMutex);
        for (NodesGuiList::iterator it = _imp->_nodes.begin(); it != _imp->_nodes.end(); ++it) {
            (*it)->setKnobLinksVisible(_imp->_knobLinksVisible);
        }
        for (NodesGuiList::iterator it = _imp->_nodesTrash.begin(); it != _imp->_nodesTrash.end(); ++it) {
            (*it)->setKnobLinksVisible(_imp->_knobLinksVisible);
        }
    }
}

void
NodeGraph::toggleAutoPreview()
{
    getGui()->getApp()->getProject()->toggleAutoPreview();
}

void
NodeGraph::forceRefreshAllPreviews()
{
    getGui()->forceRefreshAllPreviews();
}

void
NodeGraph::showMenu(const QPoint & pos)
{
    _imp->_menu->clear();

    QAction* createNode = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphCreateNode,
                                                  kShortcutDescActionGraphCreateNode, _imp->_menu);
    _imp->_menu->addAction(createNode);

    // SOLVFX: set the comp frame range from the selected Read
    QAction* solvfxRangeFromRead = 0;
    NodeGuiPtr solvfxReadNode;
    for (NodesGuiList::const_iterator it = _imp->_selection.begin(); it != _imp->_selection.end(); ++it) {
        NodePtr n = (*it)->getNode();
        if ( n && n->getEffectInstance() && n->getEffectInstance()->isReader() ) {
            solvfxReadNode = *it;
            break;
        }
    }
    if (solvfxReadNode) {
        solvfxRangeFromRead = _imp->_menu->addAction( tr("Set Comp range from this Read") );
    }
    // SOLVFX Stamps: go to / reconnect a selected Stamp
    QAction* solvfxGoToAnchorAction = 0;
    std::map<QAction*, NodeGuiPtr> solvfxReconnectActions;
    NodeGuiPtr solvfxStampNode;
    if ( _imp->_selection.size() == 1 ) {
        NodePtr n = _imp->_selection.front()->getNode();
        if ( n && solvfxIsStampID( n->getPluginID() ) ) {
            solvfxStampNode = _imp->_selection.front();
        }
    }
    {
        QAction* stampsAction = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphSolvfxStamps,
                                                       kShortcutDescActionGraphSolvfxStamps, _imp->_menu);
        QObject::connect( stampsAction, SIGNAL(triggered()), this, SLOT(solvfxStampsSlot()) );
        _imp->_menu->addAction(stampsAction);
    }
    if (solvfxStampNode) {
        solvfxGoToAnchorAction = _imp->_menu->addAction( tr("Go to anchor") );
        solvfxGoToAnchorAction->setEnabled( (bool)solvfxAnchorOfStamp(solvfxStampNode) );
        Menu* reconnectMenu = new Menu(tr("Reconnect to anchor"), _imp->_menu);
        _imp->_menu->addAction( reconnectMenu->menuAction() );
        std::vector<std::pair<std::string, NodeGuiPtr> > anchors;
        const NodesGuiList& all = getAllActiveNodes();
        for (NodesGuiList::const_iterator it = all.begin(); it != all.end(); ++it) {
            NodePtr n = (*it)->getNode();
            if ( n && solvfxIsAnchorID( n->getPluginID() ) ) {
                anchors.push_back( std::make_pair(n->getLabel(), *it) );
            }
        }
        std::sort( anchors.begin(), anchors.end(),
                   [](const std::pair<std::string, NodeGuiPtr>& a, const std::pair<std::string, NodeGuiPtr>& b) { return a.first < b.first; } );
        for (std::size_t i = 0; i < anchors.size(); ++i) {
            solvfxReconnectActions[ reconnectMenu->addAction( QString::fromUtf8( anchors[i].first.c_str() ) ) ] = anchors[i].second;
        }
        reconnectMenu->setEnabled( !anchors.empty() );
    }
    // SOLVFX: thumbnails are static; this is the only way to update them
    QAction* solvfxRefreshThumbs = _imp->_menu->addAction( _imp->_selection.empty() ? tr("Refresh thumbnails") : tr("Refresh thumbnails (selected)") );


    //QFont font(appFont,appFontSize);
    Menu* editMenu = new Menu(tr("Edit"), _imp->_menu);
    //editMenu->setFont(font);
    _imp->_menu->addAction( editMenu->menuAction() );

    QAction* copyAction = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphCopy,
                                                 kShortcutDescActionGraphCopy, editMenu);
    QObject::connect( copyAction, SIGNAL(triggered()), this, SLOT(copySelectedNodes()) );
    editMenu->addAction(copyAction);

    QAction* cutAction = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphCut,
                                                kShortcutDescActionGraphCut, editMenu);
    QObject::connect( cutAction, SIGNAL(triggered()), this, SLOT(cutSelectedNodes()) );
    editMenu->addAction(cutAction);


    QAction* pasteAction = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphPaste,
                                                  kShortcutDescActionGraphPaste, editMenu);
    pasteAction->setEnabled( !appPTR->isNodeClipBoardEmpty() );
    editMenu->addAction(pasteAction);

    QAction* deleteAction = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphRemoveNodes,
                                                   kShortcutDescActionGraphRemoveNodes, editMenu);
    QObject::connect( deleteAction, SIGNAL(triggered()), this, SLOT(deleteSelection()) );
    editMenu->addAction(deleteAction);

    QAction* renameAction = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphRenameNode,
                                                   kShortcutDescActionGraphRenameNode, editMenu);
    QObject::connect( renameAction, SIGNAL(triggered()), this, SLOT(renameNode()) );
    editMenu->addAction(renameAction);

    QAction* duplicateAction = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphDuplicate,
                                                      kShortcutDescActionGraphDuplicate, editMenu);
    editMenu->addAction(duplicateAction);

    QAction* cloneAction = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphClone,
                                                  kShortcutDescActionGraphClone, editMenu);
    editMenu->addAction(cloneAction);

    QAction* decloneAction = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphDeclone,
                                                    kShortcutDescActionGraphDeclone, editMenu);
    QObject::connect( decloneAction, SIGNAL(triggered()), this, SLOT(decloneSelectedNodes()) );
    editMenu->addAction(decloneAction);

    QAction* switchInputs = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphExtractNode,
                                                   kShortcutDescActionGraphExtractNode, editMenu);
    QObject::connect( switchInputs, SIGNAL(triggered()), this, SLOT(extractSelectedNode()) );
    editMenu->addAction(switchInputs);

    QAction* extractNode = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphSwitchInputs,
                                                  kShortcutDescActionGraphSwitchInputs, editMenu);
    QObject::connect( extractNode, SIGNAL(triggered()), this, SLOT(switchInputs1and2ForSelectedNodes()) );
    editMenu->addAction(extractNode);

    QAction* disableNodes = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphDisableNodes,
                                                   kShortcutDescActionGraphDisableNodes, editMenu);
    QObject::connect( disableNodes, SIGNAL(triggered()), this, SLOT(toggleSelectedNodesEnabled()) );
    editMenu->addAction(disableNodes);

    QAction* groupFromSel = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphMakeGroup,
                                                   kShortcutDescActionGraphMakeGroup, editMenu);
    QObject::connect( groupFromSel, SIGNAL(triggered()), this, SLOT(createGroupFromSelection()) );
    editMenu->addAction(groupFromSel);

    QAction* expandGroup = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphExpandGroup,
                                                  kShortcutDescActionGraphExpandGroup, editMenu);
    QObject::connect( expandGroup, SIGNAL(triggered()), this, SLOT(expandSelectedGroups()) );
    editMenu->addAction(expandGroup);

    QAction* findAction = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphFindNode,
                                                 kShortcutDescActionGraphFindNode, editMenu);
    editMenu->addAction(findAction);


    //QFont font(appFont,appFontSize);
    Menu* viewMenu = new Menu(tr("View"), _imp->_menu);
    //editMenu->setFont(font);
    _imp->_menu->addAction( viewMenu->menuAction() );

    QAction* displayCacheInfoAction = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphShowCacheSize,
                                                             kShortcutDescActionGraphShowCacheSize, viewMenu);
    displayCacheInfoAction->setCheckable(true);
    displayCacheInfoAction->setChecked( _imp->_cacheSizeText->isVisible() );
    QObject::connect( displayCacheInfoAction, SIGNAL(triggered()), this, SLOT(toggleCacheInfo()) );
    viewMenu->addAction(displayCacheInfoAction);

    const NodesGuiList& selectedNodes = getSelectedNodes();
    if ( !selectedNodes.empty() ) {
        QAction* turnOffPreviewAction = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphTogglePreview,
                                                               kShortcutDescActionGraphTogglePreview, viewMenu);
        turnOffPreviewAction->setCheckable(true);
        turnOffPreviewAction->setChecked(false);
        QObject::connect( turnOffPreviewAction, SIGNAL(triggered()), this, SLOT(togglePreviewsForSelectedNodes()) );
        viewMenu->addAction(turnOffPreviewAction);

        if (selectedNodes.size() == 1) {
            QAction* openNodePanelAction = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphOpenNodePanel,
                                                                  kShortcutDescActionGraphOpenNodePanel, viewMenu);
            openNodePanelAction->setCheckable(true);
            openNodePanelAction->setChecked(false);
            QObject::connect( openNodePanelAction, SIGNAL(triggered()), this, SLOT(showSelectedNodeSettingsPanel()) );
            viewMenu->addAction(openNodePanelAction);
        }
    }

    QAction* autoHideInputs = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphAutoHideInputs,
                                                     kShortcutDescActionGraphAutoHideInputs, viewMenu);
    autoHideInputs->setCheckable(true);
    autoHideInputs->setChecked( appPTR->getCurrentSettings()->areOptionalInputsAutoHidden() );
    QObject::connect( autoHideInputs, SIGNAL(triggered()), this, SLOT(toggleAutoHideInputs()) );
    viewMenu->addAction(autoHideInputs);

    QAction* hideInputs = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphHideInputs,
                                                 kShortcutDescActionGraphHideInputs, viewMenu);
    hideInputs->setCheckable(true);
    bool hideInputsVal = false;
    if ( !selectedNodes.empty() ) {
        hideInputsVal = selectedNodes.front()->getNode()->getHideInputsKnobValue();
    }
    hideInputs->setChecked(hideInputsVal);
    QObject::connect( hideInputs, SIGNAL(triggered()), this, SLOT(toggleHideInputs()) );
    viewMenu->addAction(hideInputs);


    QAction* knobLinks = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphShowExpressions,
                                                kShortcutDescActionGraphShowExpressions, viewMenu);
    knobLinks->setCheckable(true);
    knobLinks->setChecked( areKnobLinksVisible() );
    QObject::connect( knobLinks, SIGNAL(triggered()), this, SLOT(toggleKnobLinksVisible()) );
    viewMenu->addAction(knobLinks);

    QAction* autoPreview = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphToggleAutoPreview,
                                                  kShortcutDescActionGraphToggleAutoPreview, viewMenu);
    autoPreview->setCheckable(true);
    autoPreview->setChecked( getGui()->getApp()->getProject()->isAutoPreviewEnabled() );
    QObject::connect( autoPreview, SIGNAL(triggered()), this, SLOT(toggleAutoPreview()) );
    QObject::connect( getGui()->getApp()->getProject().get(), SIGNAL(autoPreviewChanged(bool)), autoPreview, SLOT(setChecked(bool)) );
    viewMenu->addAction(autoPreview);

    QAction* autoTurbo = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphToggleAutoTurbo,
                                                kShortcutDescActionGraphToggleAutoTurbo, viewMenu);
    autoTurbo->setCheckable(true);
    autoTurbo->setChecked( appPTR->getCurrentSettings()->isAutoTurboEnabled() );
    QObject::connect( autoTurbo, SIGNAL(triggered()), this, SLOT(toggleAutoTurbo()) );
    viewMenu->addAction(autoTurbo);


    QAction* forceRefreshPreviews = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphForcePreview,
                                                           kShortcutDescActionGraphForcePreview, viewMenu);
    QObject::connect( forceRefreshPreviews, SIGNAL(triggered()), this, SLOT(forceRefreshAllPreviews()) );
    viewMenu->addAction(forceRefreshPreviews);

    QAction* frameAllNodes = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphFrameNodes,
                                                    kShortcutDescActionGraphFrameNodes, viewMenu);
    QObject::connect( frameAllNodes, SIGNAL(triggered()), this, SLOT(centerOnAllNodes()) );
    viewMenu->addAction(frameAllNodes);


    _imp->_menu->addSeparator();

    std::list<ToolButton*> orederedToolButtons = getGui()->getToolButtonsOrdered();
    for (std::list<ToolButton*>::iterator it = orederedToolButtons.begin(); it != orederedToolButtons.end(); ++it) {
        (*it)->getMenu()->setIcon( (*it)->getMenuIcon() );
        _imp->_menu->addAction( (*it)->getMenu()->menuAction() );
    }

    QAction* ret = _imp->_menu->exec(pos);
    if ( ret && solvfxStampNode && (ret == solvfxGoToAnchorAction) ) {
        solvfxGoToAnchor(solvfxStampNode);
    } else if ( ret && solvfxStampNode && solvfxReconnectActions.count(ret) ) {
        solvfxReconnectStamp(solvfxStampNode, solvfxReconnectActions[ret]);
    } else if ( ret && (ret == solvfxRefreshThumbs) ) {
        const double t = getGui()->getApp()->getTimeLine()->currentFrame();
        const NodesGuiList nodes = _imp->_selection.empty() ? getAllActiveNodes() : _imp->_selection;
        for (NodesGuiList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
            if (*it) {
                (*it)->solvfxRefreshThumbnail(t);
            }
        }
    } else if ( ret && (ret == solvfxRangeFromRead) && solvfxReadNode ) {
        NodePtr n = solvfxReadNode->getNode();
        EffectInstancePtr effect = n ? n->getEffectInstance() : EffectInstancePtr();
        if (effect) {
            double first = INT_MIN, last = INT_MAX;
            effect->getFrameRange_public(effect->getHash(), &first, &last, true);
            if ( (first > INT_MIN) && (last < INT_MAX) && (last >= first) ) {
                getGui()->getApp()->getProject()->solvfxSetFrameRange( (int)first, (int)last );
            }
        }
    } else if (ret == findAction) {
        popFindDialog();
    } else if (ret == createNode) {
            popCreateNodeDialog();
    } else if (ret == duplicateAction) {
        // Duplicate at mouse click position:
        QPointF scenePos = mapToScene( mapFromGlobal(pos) );
        cloneSelectedNodes(scenePos);
    } else if (ret == cloneAction) {
        // Clone at mouse click position:
        QPointF scenePos = mapToScene( mapFromGlobal(pos) );
        cloneSelectedNodes(scenePos);
    } else if (ret == pasteAction) {
        // Paste at mouse click position:
        QPointF scenePos = mapToScene( mapFromGlobal(pos) );
        cloneSelectedNodes(scenePos);
    }
} // NodeGraph::showMenu

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_ENTER

// ---------------------------------------------------------------- SOLVFX Stamps

void
NodeGraph::solvfxStampsSlot()
{
    solvfxStampsAction();
}

NodeGuiPtr
NodeGraph::solvfxAnchorOfStamp(const NodeGuiPtr& stamp) const
{
    NodePtr n = stamp ? stamp->getNode() : NodePtr();
    NodePtr in = n ? n->getGuiInput(0) : NodePtr();
    if ( !in || !solvfxIsAnchorID( in->getPluginID() ) ) {
        return NodeGuiPtr();
    }

    return std::dynamic_pointer_cast<NodeGui>( in->getNodeGui() );
}

void
NodeGraph::solvfxGoToAnchor(const NodeGuiPtr& stamp)
{
    NodeGuiPtr anchor = solvfxAnchorOfStamp(stamp);
    if (!anchor) {
        return;
    }
    clearSelection();
    selectNode(anchor, false);
    centerOnItem( anchor.get() );
}

void
NodeGraph::solvfxReconnectStamp(const NodeGuiPtr& stamp,
                                const NodeGuiPtr& anchor)
{
    NodePtr s = stamp ? stamp->getNode() : NodePtr();
    NodePtr a = anchor ? anchor->getNode() : NodePtr();
    if (!s || !a) {
        return;
    }
    ProjectPtr project = getGui()->getApp()->getProject();
    NodePtr old = s->getGuiInput(0);
    if (old) {
        project->disconnectNodes(old, s);
    }
    project->connectNodes(0, a, s);
    s->setLabel( a->getLabel() );
    stamp->solvfxRefreshStampLook();
}

void
NodeGraph::solvfxStampsAction()
{
    if ( _imp->_selection.empty() ) {
        Dialogs::warningDialog( tr("Stamps").toStdString(),
                                tr("Select a node to create an Anchor under it, or an Anchor / Stamp to create a Stamp.").toStdString() );

        return;
    }
    NodeGuiPtr sel = _imp->_selection.front();
    NodePtr selNode = sel->getNode();
    if (!selNode) {
        return;
    }
    const std::string selID = selNode->getPluginID();
    NodeGuiPtr anchor;
    bool makeStamp = false;
    if ( solvfxIsAnchorID(selID) ) {
        anchor = sel;
        makeStamp = true;
    } else if ( solvfxIsStampID(selID) ) {
        anchor = solvfxAnchorOfStamp(sel);
        if (!anchor) {
            Dialogs::warningDialog( tr("Stamps").toStdString(), tr("This Stamp is not connected to an Anchor.").toStdString() );

            return;
        }
        makeStamp = true;
    }

    CreateNodeArgs args( makeStamp ? PLUGINID_SOLVFX_STAMP : PLUGINID_SOLVFX_ANCHOR, _imp->group.lock() );
    args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    args.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
    NodePtr created = getGui()->getApp()->createNode(args);
    if (!created) {
        return;
    }
    NodeGuiPtr createdGui = std::dynamic_pointer_cast<NodeGui>( created->getNodeGui() );
    if (!createdGui) {
        return;
    }
    ProjectPtr project = getGui()->getApp()->getProject();
    NodePtr source = makeStamp ? anchor->getNode() : selNode;
    project->connectNodes(0, source, created);

    // title: the Anchor takes the source node's name, the Stamp the Anchor's title
    created->setLabel( source->getLabel() );

    // position: the Anchor under its source; the Stamp under the mouse (or next to the selection)
    QPointF pos;
    const QPoint mouse = mapFromGlobal( QCursor::pos() );
    if ( makeStamp && rect().contains(mouse) ) {
        const QPointF scenePos = mapToScene(mouse);
        pos = createdGui->mapToParent( createdGui->mapFromScene(scenePos) );
        const QSize sz = createdGui->getSize();
        pos -= QPointF(sz.width() / 2., sz.height() / 2.);
    } else {
        const QPointF p = sel->getPos_mt_safe();
        const QSize ssz = sel->getSize();
        pos = makeStamp ? QPointF( p.x() + ssz.width() + 60, p.y() + ssz.height() + 60 ) : QPointF( p.x(), p.y() + ssz.height() + 50 );
    }
    createdGui->refreshPosition( pos.x(), pos.y(), true );
    createdGui->solvfxRefreshStampLook();

    NodesGuiList nodesList;
    nodesList.push_back(createdGui);
    pushUndoCommand( new AddMultipleNodesCommand(this, nodesList) );

    clearSelection();
    selectNode(createdGui, false);
} // NodeGraph::solvfxStampsAction

NATRON_NAMESPACE_EXIT
