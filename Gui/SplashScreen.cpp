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

#include "SplashScreen.h"

#include <ctime>
#include <stdexcept>
#include <algorithm> // std::max

CLANG_DIAG_OFF(deprecated)
#include <QPainter>
#include <QFontMetrics>
#include <QStyleOption>
#include <QApplication>

#include <QScreen>

CLANG_DIAG_ON(deprecated)

#ifdef DEBUG
#include "Global/FloatingPointExceptions.h"
#endif

NATRON_NAMESPACE_ENTER

// SOLVFX splash design (see the branding assets): the images are drawn at 2x for HiDPI
// screens; all coordinates below are in logical (1x) pixels.
#define SOLVFX_SPLASH_TEXT_COLOR QColor(149, 140, 133)
#define SOLVFX_SPLASH_DIM_TEXT_COLOR QColor(110, 102, 96)
#define SOLVFX_SPLASH_LIGHT_TEXT_COLOR QColor(237, 230, 223)
#define SOLVFX_SPLASH_ACCENT_COLOR QColor(224, 116, 58)

static void
loadSplashPixmap(QPixmap& pixmap, const QString& filePath)
{
    pixmap.load(filePath);
    // Splash images are provided at 2x resolution
    if (pixmap.width() > 1000) {
        pixmap.setDevicePixelRatio(2.);
    }
}

static QSize
splashLogicalSize(const QPixmap& pixmap)
{
    const qreal dpr = pixmap.devicePixelRatio();

    return QSize( qRound(pixmap.width() / dpr), qRound(pixmap.height() / dpr) );
}

static QFont
splashFont(int pointSize, bool bold = false)
{
    QFont f = QApplication::font();
    f.setPointSize(pointSize);
    f.setBold(bold);
    f.setStyleStrategy(QFont::PreferAntialias);

    return f;
}

SplashScreen::SplashScreen(const QString & filePath)
    : QWidget(0, Qt::FramelessWindowHint)
    , _pixmap()
    , _text()
    , _versionString()
{
    QString customBuildString = QString::fromUtf8(NATRON_CUSTOM_BUILD_USER_NAME);

    if ( customBuildString.isEmpty() ) {
        QString buildNo;
        if ( QString::fromUtf8(NATRON_DEVELOPMENT_STATUS) == QString::fromUtf8(NATRON_DEVELOPMENT_RELEASE_CANDIDATE) ) {
            buildNo = QString::number(NATRON_BUILD_NUMBER);
        }
        _versionString = tr("%1 - based on %2 v%3 %4 %5")
                         .arg( QString::fromUtf8(NATRON_APPLICATION_NAME) )
                         .arg( QString::fromUtf8(NATRON_COMPAT_APPLICATION_NAME) )
                         .arg( QString::fromUtf8(NATRON_VERSION_STRING) )
                         .arg( QString::fromUtf8(NATRON_DEVELOPMENT_STATUS) )
                         .arg(buildNo).trimmed();
#     if defined(NATRON_CONFIG_SNAPSHOT) || defined(DEBUG)
        _versionString += QString::fromUtf8(" - ") + tr("built on %1").arg( QString::fromUtf8(__DATE__) );
#     endif
    } else {
        _versionString = tr("%1 for %2")
                         .arg( QString::fromUtf8(NATRON_APPLICATION_NAME) )
                         .arg(customBuildString);
#     if defined(NATRON_CONFIG_SNAPSHOT) || defined(DEBUG)
        _versionString += QString::fromUtf8(" - ") + tr("built on %1").arg( QString::fromUtf8(__DATE__) );
#     endif
    }

    setAttribute( Qt::WA_TransparentForMouseEvents );
    setAttribute(Qt::WA_TranslucentBackground, true);

    loadSplashPixmap(_pixmap, filePath);
    _scale = 1.;
    resize( splashLogicalSize(_pixmap) );
    {
#ifdef DEBUG
        boost_adaptbx::floating_point::exception_trapping trap(0);
#endif
        show();
    }

    QScreen* desktop = QGuiApplication::primaryScreen();
    QRect screen = desktop->availableGeometry();
    move(screen.width() / 2 - width() / 2, screen.height() / 2 - height() / 2);
}

void
SplashScreen::updateText(const QString & text)
{
    _text = text;
    {
#ifdef DEBUG
        boost_adaptbx::floating_point::exception_trapping trap(0);
#endif
        update();
        QCoreApplication::processEvents();
    }
}

void
SplashScreen::paintEvent(QPaintEvent*)
{
    QStyleOption opt;

    opt.init(this);
    QPainter p(this);

    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);

    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.drawPixmap(0, 0, _pixmap);

    // Status text in the empty band under the credit line, version on the right,
    // both aligned with the margins of the artwork (52 px left, 52 px right).
    const QSize size = splashLogicalSize(_pixmap);
    const int margin = 52;
    const int textY = 404;
    const QRect textRect(margin, textY, size.width() - 2 * margin, 20);
    p.setFont( splashFont(8) );
    p.setPen(SOLVFX_SPLASH_DIM_TEXT_COLOR);
    p.drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, _versionString);
    p.setPen(SOLVFX_SPLASH_TEXT_COLOR);
    const int versionWidth = QFontMetrics( p.font() ).horizontalAdvance(_versionString);
    const QRect statusRect( margin, textY, std::max(0, textRect.width() - versionWidth - 20), 20 );
    p.drawText(statusRect, Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetrics( p.font() ).elidedText(_text, Qt::ElideRight, statusRect.width()) );
}

LoadProjectSplashScreen::LoadProjectSplashScreen(const QString & filePath)
    : QWidget(0, Qt::SplashScreen)
    , _pixmap()
    , _text()
    , _projectName(filePath)
{
    setAttribute( Qt::WA_TransparentForMouseEvents );
    setAttribute(Qt::WA_TranslucentBackground, true);

    loadSplashPixmap( _pixmap, QString::fromUtf8(":Resources/Images/loadProjectSplashscreen.png") );

    _scale = 1.;
    resize( splashLogicalSize(_pixmap) );
    show();

    QScreen* desktop = QGuiApplication::primaryScreen();
    QRect screen = desktop->availableGeometry();
    move(screen.width() / 2 - width() / 2, screen.height() / 2 - height() / 2);
}

void
LoadProjectSplashScreen::updateText(const QString & text)
{
    _text = text;
    update();
    {
#ifdef DEBUG
        boost_adaptbx::floating_point::exception_trapping trap(0);
#endif
        QCoreApplication::processEvents();
    }
}

void
LoadProjectSplashScreen::paintEvent(QPaintEvent* /*e*/)
{
    QStyleOption opt;

    opt.init(this);
    QPainter p(this);

    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);

    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.drawPixmap(0, 0, _pixmap);

    // "Loading <project>" and the status text go under the track line of the artwork
    const QSize size = splashLogicalSize(_pixmap);
    const int margin = 44;
    const int width = size.width() - 2 * margin;

    p.setFont( splashFont(10, true) );
    const QString loadString( tr("Loading") + QLatin1Char(' ') );
    const QFontMetrics fmBold( p.font() );
    const int loadWidth = fmBold.horizontalAdvance(loadString);
    const QRect loadRect(margin, 238, width, 22);
    p.setPen(SOLVFX_SPLASH_ACCENT_COLOR);
    p.drawText(loadRect, Qt::AlignLeft | Qt::AlignVCenter, loadString);
    p.setPen(SOLVFX_SPLASH_LIGHT_TEXT_COLOR);
    p.drawText(loadRect.adjusted(loadWidth, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter,
               fmBold.elidedText(_projectName, Qt::ElideMiddle, width - loadWidth) );

    p.setFont( splashFont(8) );
    p.setPen(SOLVFX_SPLASH_TEXT_COLOR);
    const QRect statusRect(margin, 264, width, 18);
    p.drawText(statusRect, Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetrics( p.font() ).elidedText(_text, Qt::ElideRight, width) );
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_SplashScreen.cpp"
