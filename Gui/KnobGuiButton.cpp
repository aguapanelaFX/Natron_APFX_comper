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

#include "KnobGuiButton.h"

#include <cfloat>
#include <algorithm> // min, max
#include <stdexcept>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QGridLayout>
#include <QHBoxLayout>
#include <QStyle>
#include <QColorDialog>
#include <QToolTip>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QHeaderView>
#include <QApplication>
#include <QScrollArea>
GCC_DIAG_UNUSED_PRIVATE_FIELD_OFF
// /opt/local/include/QtGui/qmime.h:119:10: warning: private field 'type' is not used [-Wunused-private-field]
#include <QKeyEvent>
GCC_DIAG_UNUSED_PRIVATE_FIELD_ON
#include <QDebug>
#include <QFontComboBox>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Lut.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"

#include "Gui/Button.h"
#include "Gui/ClickableLabel.h"
#include "Gui/ComboBox.h"
#include "Gui/CurveGui.h"
#include "Gui/DockablePanel.h"
#include "Gui/GroupBoxLabel.h"
#include "Gui/Gui.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/GuiDefines.h"
#include "Gui/GuiMacros.h"
#include "Gui/KnobUndoCommand.h"
#include "Gui/Label.h"
#include "Gui/NewLayerDialog.h"
#include "Gui/ProjectGui.h"
#include "Gui/ScaleSliderQWidget.h"
#include "Gui/SpinBox.h"
#include "Gui/TabGroup.h"

#include <ofxNatron.h>
#include <QImage>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QMessageBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QComboBox>
#include <QRegExp>
#include <climits>

#include "Engine/AppInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/WriteNode.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/TimeLineGui.h"
#include "Gui/ViewerTab.h"
#include <algorithm>

NATRON_NAMESPACE_ENTER
using std::make_pair;


// SOLVFX: one-gray icon (shape from the alpha and luminance), brighter when checked
static QPixmap
solvfxGrayIcon(const QPixmap& src, bool on)
{
    QImage im = src.toImage().convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < im.height(); ++y) {
        QRgb* line = (QRgb*)im.scanLine(y);
        for (int x = 0; x < im.width(); ++x) {
            const int a = qAlpha(line[x]);
            const int lum = qGray(line[x]);
            const int g = on ? 235 : std::min(215, 150 + lum / 4);
            line[x] = qRgba(g, g, g, a);
        }
    }

    return QPixmap::fromImage(im);
}

//=============================BUTTON_KNOB_GUI===================================

KnobGuiButton::KnobGuiButton(KnobIPtr knob,
                             KnobGuiContainerI *container)
    : KnobGui(knob, container)
    , _button(0)
{
    _knob = std::dynamic_pointer_cast<KnobButton>(knob);
}

void
KnobGuiButton::createWidget(QHBoxLayout* layout)
{
    KnobButtonPtr knob = _knob.lock();
    QString label = QString::fromUtf8( knob->getLabel().c_str() );
    QString onIconFilePath = QString::fromUtf8( knob->getIconLabel(true).c_str() );
    QString offIconFilePath = QString::fromUtf8( knob->getIconLabel(false).c_str() );


    if ( !onIconFilePath.isEmpty() && !QFile::exists(onIconFilePath) ) {
        EffectInstance* isEffect = dynamic_cast<EffectInstance*>( knob->getHolder() );
        if (isEffect) {
            //Prepend the resources path
            QString resourcesPath = QString::fromUtf8( isEffect->getNode()->getPluginResourcesPath().c_str() );
            if ( !resourcesPath.endsWith( QLatin1Char('/') ) ) {
                resourcesPath += QLatin1Char('/');
            }
            onIconFilePath.prepend(resourcesPath);
        }
    }
    if ( !offIconFilePath.isEmpty() && !QFile::exists(offIconFilePath) ) {
        EffectInstance* isEffect = dynamic_cast<EffectInstance*>( knob->getHolder() );
        if (isEffect) {
            //Prepend the resources path
            QString resourcesPath = QString::fromUtf8( isEffect->getNode()->getPluginResourcesPath().c_str() );
            if ( !resourcesPath.endsWith( QLatin1Char('/') ) ) {
                resourcesPath += QLatin1Char('/');
            }
            offIconFilePath.prepend(resourcesPath);
        }
    }

    bool checkable = knob->getIsCheckable();
    // SOLVFX: viewer toolbars (Tracker, ...) use one gray like Nuke, a bit larger
    // icon buttons are one gray everywhere (viewer bars and panels), 22 px
    const bool inViewer = true;
    QIcon icon;
    QPixmap pixChecked, pixUnchecked;
    if ( !offIconFilePath.isEmpty() ) {
        if ( pixUnchecked.load(offIconFilePath) ) {
            if (inViewer) {
                pixUnchecked = solvfxGrayIcon(pixUnchecked, false);
            }
            icon.addPixmap(pixUnchecked, QIcon::Normal, QIcon::Off);
        }
    }
    if ( !onIconFilePath.isEmpty() ) {
        if ( pixChecked.load(onIconFilePath) ) {
            if (inViewer) {
                pixChecked = solvfxGrayIcon(pixChecked, true);
            }
            icon.addPixmap(pixChecked, QIcon::Normal, QIcon::On);
        }
    }

    if ( !icon.isNull() ) {
        _button = new Button( icon, QString(), layout->parentWidget() );
        if (inViewer) {
            _button->setFixedSize( TO_DPIX(22), TO_DPIY(22) );
            _button->setIconSize( QSize( TO_DPIX(18), TO_DPIY(18) ) );
        } else {
            _button->setFixedSize(NATRON_MEDIUM_BUTTON_SIZE, NATRON_MEDIUM_BUTTON_SIZE);
            _button->setIconSize( QSize(NATRON_MEDIUM_BUTTON_ICON_SIZE, NATRON_MEDIUM_BUTTON_ICON_SIZE) );
        }
    } else {
        _button = new Button( label, layout->parentWidget() );
        // SOLVFX: text buttons never clip their label
        _button->setMinimumHeight( TO_DPIY(20) );
        _button->setStyleSheet( QString::fromUtf8("padding: 1px 6px;") );
        // SOLVFX: compact "normalize" button, sitting at the end of the checkbox row
        if (knob->getName() == "normalize") {
            QFont f = _button->font();
            f.setPointSizeF( std::max( 6., f.pointSizeF() * 0.85 ) );
            _button->setFont(f);
            _button->setFixedHeight( TO_DPIY(17) );
            _button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            _button->setStyleSheet( QString::fromUtf8("padding: 0px 8px;") );
            layout->addSpacing( TO_DPIX(10) );
        }
    }
    if (checkable) {
        _button->setCheckable(true);
        bool checked = knob->getValue();
        _button->setChecked(checked);
        _button->setDown(checked);
    }
    QObject::connect( _button, SIGNAL(clicked(bool)), this, SLOT(emitValueChanged(bool)) );
    if ( hasToolTip() ) {
        _button->setToolTip( toolTip() );
    }
    layout->addWidget(_button);
} // KnobGuiButton::createWidget

std::string
KnobGuiButton::getDescriptionLabel() const
{
    return std::string();
}

KnobGuiButton::~KnobGuiButton()
{
}

void
KnobGuiButton::removeSpecificGui()
{
    _button->deleteLater();
}

// SOLVFX: Nuke-like render dialog: frame range presets + editable frame list, overwrite confirmation,
// timeline cursor following the rendered frame
static int g_solvfxRenderPreset = 1; // 0 global, 1 input, 2 in-out / visible, 3 custom

static bool
solvfxParseFrames(const QString& text,
                  std::list<AppInstance::RenderWork>* works,
                  OutputEffectInstance* writer,
                  bool stats)
{
    const QStringList parts = text.split(QRegExp(QString::fromUtf8("[,\\s]+")), QString::SkipEmptyParts);
    QRegExp re( QString::fromUtf8("^(-?\\d+)(?:-(-?\\d+))?(?:[xX](\\d+))?$") );
    for (int i = 0; i < parts.size(); ++i) {
        if ( re.indexIn(parts[i]) < 0 ) {
            return false;
        }
        const int a = re.cap(1).toInt();
        const int b = re.cap(2).isEmpty() ? a : re.cap(2).toInt();
        const int step = re.cap(3).isEmpty() ? 1 : std::max(1, re.cap(3).toInt());
        works->push_back( AppInstance::RenderWork( writer, std::min(a, b), std::max(a, b), step, stats ) );
    }

    return !works->empty();
}

static void
solvfxRenderDialog(Gui* gui,
                   EffectInstance* effect)
{
    OutputEffectInstance* writer = dynamic_cast<OutputEffectInstance*>(effect);
    if (!gui || !writer) {
        return;
    }
    // ranges
    int ranges[3][2];
    {
        double f = 1, l = 1;
        gui->getApp()->getFrameRange(&f, &l);
        ranges[0][0] = (int)f; ranges[0][1] = (int)l;
        double fi = f, li = l;
        EffectInstancePtr in = effect->getInput(0);
        if (in) {
            in->getFrameRange_public(in->getHash(), &fi, &li);
        }
        ranges[1][0] = (int)fi; ranges[1][1] = (int)li;
        int vl = (int)f, vr = (int)l;
        ViewerTab* v = gui->getActiveViewer();
        if (v) {
            v->getTimelineBounds(&vl, &vr);
        }
        ranges[2][0] = vl; ranges[2][1] = vr;
    }
    QDialog dlg(gui);
    dlg.setWindowTitle( QString::fromUtf8("Render %1").arg( QString::fromUtf8( effect->getNode()->getLabel().c_str() ) ) );
    QFormLayout* form = new QFormLayout(&dlg);
    QComboBox* preset = new QComboBox(&dlg);
    preset->addItem( QString::fromUtf8("global") );
    preset->addItem( QString::fromUtf8("input") );
    preset->addItem( QString::fromUtf8("in-out / visible") );
    preset->addItem( QString::fromUtf8("custom") );
    QLineEdit* frames = new QLineEdit(&dlg);
    frames->setToolTip( QString::fromUtf8("Frames to render, e.g. 1-100, 120, 200-300x2") );
    form->addRow(QString::fromUtf8("Frame range"), preset);
    form->addRow(QString::fromUtf8("Frames"), frames);
    QDialogButtonBox* bb = new QDialogButtonBox(&dlg);
    bb->addButton(QString::fromUtf8("Render"), QDialogButtonBox::AcceptRole);
    bb->addButton(QDialogButtonBox::Cancel);
    form->addRow(bb);
    QObject::connect( bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept );
    QObject::connect( bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject );
    bool filling = false;
    auto fill = [&](int i) {
        if ( (i >= 0) && (i < 3) ) {
            filling = true;
            frames->setText( QString::fromUtf8("%1-%2").arg(ranges[i][0]).arg(ranges[i][1]) );
            filling = false;
        }
    };
    QObject::connect( preset, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), fill );
    QObject::connect( frames, &QLineEdit::textEdited, [&](const QString&) {
        if (!filling) {
            preset->blockSignals(true);
            preset->setCurrentIndex(3);
            preset->blockSignals(false);
        }
    } );
    static QString lastCustom = QString::fromUtf8("1-100");
    // SOLVFX: always opens on the global range (Nuke)
    preset->blockSignals(true);
    preset->setCurrentIndex(0);
    preset->blockSignals(false);
    fill(0);
    Q_UNUSED(lastCustom);
    frames->setFocus();
    frames->selectAll();
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    g_solvfxRenderPreset = preset->currentIndex();
    if (g_solvfxRenderPreset == 3) {
        lastCustom = frames->text();
    }
    std::list<AppInstance::RenderWork> works;
    if ( !solvfxParseFrames( frames->text(), &works, writer, gui->getApp()->isRenderStatsActionChecked() ) ) {
        QMessageBox::warning( gui, QString::fromUtf8("Render"), QString::fromUtf8("Invalid frame range: %1").arg( frames->text() ) );

        return;
    }

    // overwrite check
    KnobBoolPtr overwrite;
    KnobOutputFilePtr file;
    {
        WriteNode* wn = dynamic_cast<WriteNode*>(effect);
        NodePtr embedded = wn ? wn->getEmbeddedWriter() : NodePtr();
        EffectInstance* holders[2] = { effect, embedded ? embedded->getEffectInstance().get() : 0 };
        for (int h = 0; h < 2; ++h) {
            if (!holders[h]) {
                continue;
            }
            if (!overwrite) {
                overwrite = std::dynamic_pointer_cast<KnobBool>( holders[h]->getKnobByName("overwrite") );
            }
            if (!file) {
                file = std::dynamic_pointer_cast<KnobOutputFile>( holders[h]->getKnobByName(kOfxImageEffectFileParamName) );
            }
        }
    }
    bool turnedOverwriteOn = false;
    if ( file && overwrite && !overwrite->getValue() ) {
        int existing = 0;
        QString firstExisting;
        std::set<QString> checked;
        for (std::list<AppInstance::RenderWork>::iterator it = works.begin(); it != works.end(); ++it) {
            for (int t = it->firstFrame; t <= it->lastFrame; t += std::max(1, it->frameStep)) {
                std::string fs = file->generateFileNameAtTime( t, ViewSpec(0) ).toStdString();
                gui->getApp()->getProject()->canonicalizePath(fs); // variables ($OUT, [Project] ...)
                const QString fn = QString::fromUtf8( fs.c_str() );
                if ( checked.insert(fn).second && QFileInfo(fn).exists() ) {
                    if (existing == 0) {
                        firstExisting = fn;
                    }
                    ++existing;
                }
            }
        }
        if (existing > 0) {
            QMessageBox box(gui);
            box.setIcon(QMessageBox::Warning);
            box.setWindowTitle( QString::fromUtf8("Overwrite") );
            box.setText( QString::fromUtf8("%1 file(s) already exist and will be overwritten:\n%2").arg(existing).arg(firstExisting) );
            QPushButton* ow = box.addButton(QString::fromUtf8("Overwrite"), QMessageBox::AcceptRole);
            box.addButton(QMessageBox::Cancel);
            box.exec();
            if (box.clickedButton() != ow) {
                return;
            }
            overwrite->setValue(true);
            turnedOverwriteOn = true;
        }
    }

    gui->getApp()->startWritersRendering(false, works);

    // timeline cursor follows the rendered frame; overwrite back off when done
    RenderEnginePtr engine = writer->getRenderEngine();
    if (engine) {
        QObject* ctx = new QObject(gui);
        QObject::connect( engine.get(), &RenderEngine::frameRendered, ctx, [](int time, double) {
            TimeLineGui::solvfxSetRenderFrame(time);
        } );
        QObject::connect( engine.get(), &RenderEngine::renderFinished, ctx, [ctx, overwrite, turnedOverwriteOn](int) {
            TimeLineGui::solvfxSetRenderFrame(INT_MIN);
            if (turnedOverwriteOn && overwrite) {
                overwrite->setValue(false);
            }
            ctx->deleteLater();
        } );
    } else if (turnedOverwriteOn && overwrite) {
        // no engine to watch: leave it on for this render
    }
} // solvfxRenderDialog

void
KnobGuiButton::emitValueChanged(bool clicked)
{
    KnobButtonPtr k = _knob.lock();

    assert(k);
    if ( !k->getIsCheckable() && k->isRenderButton() ) {
        EffectInstance* fx = dynamic_cast<EffectInstance*>( k->getHolder() );
        if ( fx && fx->isWriter() ) {
            solvfxRenderDialog(getGui(), fx);

            return;
        }
    }

    if ( k->getIsCheckable() ) {
        _button->setDown(clicked);
        _button->setChecked(clicked);

        pushUndoCommand( new KnobUndoCommand<bool>(shared_from_this(), k->getValue(), clicked, 0, false) );
    } else {
        k->trigger();
    }
}

void
KnobGuiButton::_hide()
{
    if (_button) {
        _button->hide();
    }
}

void
KnobGuiButton::_show()
{
    if (_button) {
        _button->show();
    }
}

void
KnobGuiButton::updateGUI(int /*dimension*/)
{
    KnobButtonPtr k = _knob.lock();

    if ( k->getIsCheckable() ) {
        bool checked = k->getValue();
        _button->setDown(checked);
        _button->setChecked(checked);
    }
}

void
KnobGuiButton::setEnabled()
{
    KnobButtonPtr knob = _knob.lock();
    bool b = knob->isEnabled(0);

    _button->setEnabled(b);
}

void
KnobGuiButton::setReadOnly(bool readOnly,
                           int /*dimension*/)
{
    _button->setEnabled(!readOnly);
}

KnobIPtr
KnobGuiButton::getKnob() const
{
    return _knob.lock();
}

void
KnobGuiButton::onLabelChangedInternal()
{
    if (_button) {
        _button->setText(QString::fromUtf8(getKnob()->getLabel().c_str()));
    }
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_KnobGuiButton.cpp"
