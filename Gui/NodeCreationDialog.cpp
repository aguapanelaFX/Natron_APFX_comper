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

#include "NodeCreationDialog.h"

#include <stdexcept>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QVBoxLayout>
#include <QStringList>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPolygonF>
#include <cmath>
#include <algorithm>
#include <QAbstractItemView>
#include <QTimer>
#include <QApplication>
#include <QListView>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QSettings>
#include <QDesktopWidget>
#include <QRegExp>
#include <QApplication>
#include <QStringListModel>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppManager.h"
#include "Engine/Plugin.h"
#include "Gui/LineEdit.h"
#include "Gui/GuiApplicationManager.h"

/*
   If defined, the list will contain matches ordered by increasing match length of the regexp
   otherwise they will be ordered with their score
 */
//#define NODE_TAB_DIALOG_USE_MATCHED_LENGTH

NATRON_NAMESPACE_ENTER


class SolvfxNodeSearchDelegate;

struct CompleterLineEditPrivate
{
    QDialog* dialog;
    QListView* listView;
    QStringListModel* model;
    CompleterLineEdit::PluginsNamesMap names;
    bool quickExitEnabled;
    SolvfxNodeSearchDelegate* delegate = 0;

    CompleterLineEditPrivate(const CompleterLineEdit::PluginsNamesMap& plugins,
                             bool quickExit,
                             QDialog* parent)
        : dialog(parent)
        , listView(NULL)
        , model(NULL)
        , names(plugins)
        , quickExitEnabled(quickExit)
    {
    }

    CompleterLineEditPrivate(const QStringList & displayWords,
                             const QStringList & internalIds,
                             bool quickExit,
                             QDialog* parent)
        : dialog(parent)
        , listView(NULL)
        , model(NULL)
        , quickExitEnabled(quickExit)
    {
        assert( displayWords.size() == internalIds.size() );

        for (int i = 0; i < displayWords.size(); ++i) {
            names.insert( std::make_pair( 0, std::make_pair(internalIds[i], displayWords[i]) ) );
        }
    }
};

// SOLVFX: Tab node search, laid out like the node search of other compositors: the node name with the
// typed letters in bold, its category in [brackets] after it, usage dots and a favourite star on the right
// (click the star: favourites come first).
static QStringList
solvfxTabFavourites()
{
    QSettings settings( QString::fromUtf8(NATRON_ORGANIZATION_NAME), QString::fromUtf8(NATRON_APPLICATION_NAME) );

    return settings.value( QString::fromUtf8("solvfx_tab_favourites") ).toStringList();
}

static void
solvfxSetTabFavourites(const QStringList& l)
{
    QSettings settings( QString::fromUtf8(NATRON_ORGANIZATION_NAME), QString::fromUtf8(NATRON_APPLICATION_NAME) );
    settings.setValue( QString::fromUtf8("solvfx_tab_favourites"), l );
}

// "Name  [Group/Sub]" -> name, top category
static void
solvfxSplitEntry(const QString& full, QString* name, QString* cat)
{
    *name = full;
    cat->clear();
    const int br = full.lastIndexOf( QString::fromUtf8("  [") );
    if (br >= 0) {
        *name = full.left(br);
        *cat = full.mid(br + 3);
        if ( cat->endsWith( QLatin1Char(']') ) ) {
            cat->chop(1);
        }
        const int slash = cat->indexOf( QLatin1Char('/') );
        if (slash >= 0) {
            *cat = cat->left(slash);
        }
    }
}

class SolvfxNodeSearchDelegate
    : public QStyledItemDelegate
{
    QLineEdit* _edit;
    bool _stars;                       // node search (favourites, usage dots)
    bool _starHit = false;

public:
    QStringList favourites;
    std::map<QString, int> weights;    // name -> times created

    SolvfxNodeSearchDelegate(QLineEdit* edit, bool stars)
        : QStyledItemDelegate(edit)
        , _edit(edit)
        , _stars(stars)
    {
        if (_stars) {
            favourites = solvfxTabFavourites();
        }
    }

    bool takeStarHit()
    {
        const bool h = _starHit;
        _starHit = false;

        return h;
    }

    static QRect starRect(const QRect& row)
    {
        const int s = (int)TO_DPIX(14);

        return QRect( row.right() - s - (int)TO_DPIX(8), row.center().y() - s / 2, s, s );
    }

    virtual QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const OVERRIDE FINAL
    {
        QSize sz = QStyledItemDelegate::sizeHint(option, index);
        sz.setHeight( std::max( sz.height(), (int)TO_DPIY(22) ) );

        return sz;
    }

    virtual bool editorEvent(QEvent* e, QAbstractItemModel* model, const QStyleOptionViewItem& option, const QModelIndex& index) OVERRIDE FINAL
    {
        if ( _stars && (e->type() == QEvent::MouseButtonPress) ) {
            QMouseEvent* me = static_cast<QMouseEvent*>(e);
            if ( starRect(option.rect).adjusted(-4, -4, 4, 4).contains( me->pos() ) ) {
                QString name, cat;
                solvfxSplitEntry(index.data().toString(), &name, &cat);
                if ( favourites.contains(name) ) {
                    favourites.removeAll(name);
                } else {
                    favourites.append(name);
                }
                solvfxSetTabFavourites(favourites);
                _starHit = true;

                return true;
            }
        }

        return QStyledItemDelegate::editorEvent(e, model, option, index);
    }

    virtual void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const OVERRIDE FINAL
    {
        QString name, cat;
        solvfxSplitEntry(index.data().toString(), &name, &cat);
        const QRect r = option.rect;
        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        if (selected || hovered) {
            p->setPen(Qt::NoPen);
            p->setBrush( selected ? QColor(0x4d, 0x4d, 0x4d) : QColor(0x36, 0x36, 0x36) );
            p->drawRect(r);
        }
        // name: the typed letters (in order) bold and brighter
        QFont f = option.font;
        QFont fb = f;
        fb.setBold(true);
        const QString typed = _edit ? _edit->text().toLower() : QString();
        int ti = 0;
        double x = r.left() + TO_DPIX(8);
        const QColor txt = selected ? QColor(0xf2, 0xf2, 0xf2) : QColor(0xc8, 0xc8, 0xc8);
        const QColor hit = QColor(0xff, 0xff, 0xff);
        const double base = r.center().y() + QFontMetrics(f).ascent() / 2. - 1;
        for (int i = 0; i < name.size(); ++i) {
            const bool match = ( ti < typed.size() ) && ( name[i].toLower() == typed[ti] );
            if (match) {
                ++ti;
            }
            p->setFont(match ? fb : f);
            p->setPen(match ? hit : txt);
            QFontMetrics fm( match ? fb : f );
            p->drawText( QPointF(x, base), QString(name[i]) );
            x += fm.horizontalAdvance(name[i]);
        }
        // [category] right after the name
        if ( !cat.isEmpty() ) {
            p->setFont(f);
            p->setPen( selected ? QColor(0xa8, 0xa8, 0xa8) : QColor(0x86, 0x86, 0x86) );
            p->drawText( QPointF( x + TO_DPIX(5), base ), QString::fromUtf8("[") + cat + QString::fromUtf8("]") );
        }
        if (_stars) {
            // usage dots (how often the node is created) and the favourite star
            int w = 0;
            std::map<QString, int>::const_iterator wi = weights.find(name);
            if ( wi != weights.end() ) {
                w = wi->second;
            }
            const int dots = (w >= 25) ? 3 : (w >= 8) ? 2 : (w >= 1) ? 1 : 0;
            const QRect sr = starRect(r);
            p->setPen(Qt::NoPen);
            p->setBrush( QColor(0x9a, 0x9a, 0x9a) );
            for (int d = 0; d < dots; ++d) {
                const double cx = sr.left() - TO_DPIX(10) - d * TO_DPIX(5), cy = r.center().y();
                p->drawEllipse( QPointF(cx, cy), TO_DPIX(1.3), TO_DPIX(1.3) );
            }
            const bool fav = favourites.contains(name);
            QPolygonF star;
            const QPointF c = QRectF(sr).center();
            const double ro = sr.width() * 0.5, ri = ro * 0.45;
            for (int k = 0; k < 10; ++k) {
                const double a = -1.5707963 + k * 3.14159265 / 5.;
                const double rr = (k % 2) ? ri : ro;
                star << QPointF( c.x() + rr * std::cos(a), c.y() + rr * std::sin(a) );
            }
            if (fav) {
                p->setPen(Qt::NoPen);
                p->setBrush( QColor(0xff, 0xd2, 0x57) );
            } else {
                p->setPen( QPen(QColor(0x6e, 0x6e, 0x6e), 1.) );
                p->setBrush(Qt::NoBrush);
            }
            p->drawPolygon(star);
        }
        p->restore();
    }
};

static SolvfxNodeSearchDelegate*
solvfxStyleSearchList(QListView* view, QLineEdit* edit, bool stars)
{
    SolvfxNodeSearchDelegate* d = new SolvfxNodeSearchDelegate(edit, stars);
    view->setItemDelegate(d);
    view->setMouseTracking(true);
    view->setStyleSheet( QString::fromUtf8(
        "QListView { background-color: #262626; border: 1px solid #111111; outline: 0; padding: 0px; }"
        "QScrollBar:vertical { background: #262626; width: 8px; margin: 0px; }"
        "QScrollBar::handle:vertical { background: #4a4a4a; min-height: 24px; border-radius: 3px; }"
        "QScrollBar::handle:vertical:hover { background: #5c5c5c; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }") );
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setMinimumWidth( TO_DPIX(320) );

    return d;
}

// how well a node name matches the typed text (-1: no match). Exact > starts with > initials (DM: DeepMerge)
// > contains > letters in order.
static int
solvfxMatchScore(const QString& name, const QString& typed)
{
    const QString n = name.toLower(), t = typed.toLower();
    if ( t.isEmpty() ) {
        return 0;
    }
    if (n == t) {
        return 1000;
    }
    if ( n.startsWith(t) ) {
        return 800 - std::min( 100, (int)( n.size() - t.size() ) );
    }
    // initials: first letter, capitals, letters after a separator or a digit run
    QString ini;
    for (int i = 0; i < name.size(); ++i) {
        const QChar c = name[i];
        const bool start = (i == 0) || c.isUpper() || !name[i - 1].isLetterOrNumber() || ( c.isDigit() && !name[i - 1].isDigit() );
        if ( start && c.isLetterOrNumber() ) {
            ini += c.toLower();
        }
    }
    if ( ini.startsWith(t) ) {
        return 700 - std::min( 100, (int)( ini.size() - t.size() ) );
    }
    const int idx = (int)n.indexOf(t);
    if (idx >= 0) {
        return 500 - std::min( 100, (int)idx );
    }
    int j = 0, gaps = 0, last = -1;
    for (int i = 0; i < n.size() && j < t.size(); ++i) {
        if (n[i] == t[j]) {
            if ( (last >= 0) && (i != last + 1) ) {
                ++gaps;
            }
            last = i;
            ++j;
        }
    }

    return (j == t.size()) ? 200 - std::min(100, gaps * 10) : -1;
}

CompleterLineEdit::CompleterLineEdit(const PluginsNamesMap& plugins,
                                     bool quickExit,
                                     QDialog* parent)
    : LineEdit(parent)
    , _imp( new CompleterLineEditPrivate(plugins, quickExit, parent) )
{
    _imp->listView = new QListView(parent);
    _imp->model = new QStringListModel(this);
    _imp->listView->setWindowFlags(Qt::ToolTip);
    _imp->listView->setModel(_imp->model);
    _imp->delegate = solvfxStyleSearchList(_imp->listView, this, true); // SOLVFX
    for (PluginsNamesMap::const_iterator it = _imp->names.begin(); it != _imp->names.end(); ++it) {
        QString name, cat;
        solvfxSplitEntry(it->second.second, &name, &cat);
        _imp->delegate->weights[name] = it->first;
    }

    connect( this, SIGNAL(textChanged(QString)), this, SLOT(filterText(QString)) );
    connect( _imp->listView, SIGNAL(clicked(QModelIndex)), this, SLOT(setTextFromIndex(QModelIndex)) );
}

CompleterLineEdit::CompleterLineEdit(const QStringList & displayWords,
                                     const QStringList & internalIds,
                                     bool quickExit,
                                     QDialog* parent)
    : LineEdit(parent)
    , _imp( new CompleterLineEditPrivate(displayWords, internalIds, quickExit, parent) )
{
    _imp->listView = new QListView(parent);
    _imp->model = new QStringListModel(this);
    _imp->listView->setWindowFlags(Qt::ToolTip);
    _imp->listView->setModel(_imp->model);
    _imp->delegate = solvfxStyleSearchList(_imp->listView, this, false); // SOLVFX

    connect( this, SIGNAL(textChanged(QString)), this, SLOT(filterText(QString)) );
    connect( _imp->listView, SIGNAL(clicked(QModelIndex)), this, SLOT(setTextFromIndex(QModelIndex)) );
}

CompleterLineEdit::~CompleterLineEdit()
{
}

QListView*
CompleterLineEdit::getView() const
{
    return _imp->listView;
}

void
CompleterLineEdit::filterText(const QString & txt)
{
    QStringList sl;
    // SOLVFX: ranked like other compositors: favourites, then how well the name matches, then how often the
    // node is used
    struct Hit { int score; QString name, display; };
    std::vector<Hit> hits;
    const QStringList favs = _imp->delegate ? _imp->delegate->favourites : QStringList();
    for (PluginsNamesMap::iterator it = _imp->names.begin(); it != _imp->names.end(); ++it) {
        QString name, cat;
        solvfxSplitEntry(it->second.second, &name, &cat);
        const QString key = it->second.first.isEmpty() ? name : it->second.first;
        int sc = txt.isEmpty() ? 0 : solvfxMatchScore(key, txt);
        if (sc < 0) {
            continue;
        }
        sc += std::min(50, it->first) * 2;
        if ( favs.contains(name) ) {
            sc += 2000;
        }
        Hit h;
        h.score = sc;
        h.name = name;
        h.display = it->second.second;
        hits.push_back(h);
    }
    std::stable_sort( hits.begin(), hits.end(), [](const Hit& x, const Hit& y) {
        return (x.score != y.score) ? (x.score > y.score) : (x.name.compare(y.name, Qt::CaseInsensitive) < 0);
    } );
    for (const Hit& h : hits) {
        sl.push_back(h.display);
    }
    _imp->model->setStringList(sl);
    _imp->listView->setModel(_imp->model);

    int rowCount = _imp->model->rowCount();
    if (rowCount == 0) {
        return;
    }

    QPoint p = mapToGlobal( QPoint( 0, height() ) );
    //QDesktopWidget* desktop = QApplication::desktop();
    //QRect screen = desktop->screenGeometry();
    //double maxHeight = ( screen.height() - p.y() ) * 0.8;
    //QFontMetrics fm = _imp->listView->fontMetrics();
    //maxHeight = std::min( maxHeight, ( rowCount * fm.height() * 1.2 + fm.height() ) );

    // Position the text edit
    //   _imp->listView->setFixedSize(width(),maxHeight);

    _imp->listView->move(p);
    _imp->listView->show();
} // CompleterLineEdit::filterText

void
CompleterLineEdit::setTextFromIndex(const QModelIndex & index)
{
    if ( _imp->delegate && _imp->delegate->takeStarHit() ) {
        // SOLVFX: the click was on the favourite star: re-rank, keep the search open
        const int row = index.row();
        filterText( text() );
        if ( _imp->model->rowCount() > 0 ) {
            _imp->listView->setCurrentIndex( _imp->model->index( std::min(row, _imp->model->rowCount() - 1) ) );
        }

        return;
    }
    QString text = index.data().toString();

    setText(text);
    Q_EMIT itemCompletionChosen();
    _imp->listView->hide();
    if (_imp->quickExitEnabled) {
        NodeCreationDialog* isNodeDialog = dynamic_cast<NodeCreationDialog*>(_imp->dialog);
        if (isNodeDialog) {
            isNodeDialog->finish(true);
        } else {
            if (_imp->dialog) {
                _imp->dialog->accept();
            }
        }
    }
}

void
CompleterLineEdit::keyPressEvent(QKeyEvent* e)
{
    int key = e->key();
    bool viewVisible = !_imp->listView->isHidden();

    assert( _imp->listView->model() );
    int count = _imp->listView->model()->rowCount();
    QModelIndex currentIndex = _imp->listView->currentIndex();

    if (key == Qt::Key_Escape) {
        if (_imp->quickExitEnabled) {
            NodeCreationDialog* isNodeDialog = dynamic_cast<NodeCreationDialog*>(_imp->dialog);
            if (isNodeDialog) {
                isNodeDialog->finish(false);
            } else {
                if (_imp->dialog) {
                    _imp->dialog->accept();
                }
            }
        } else {
            if ( _imp->listView->isVisible() ) {
                _imp->listView->hide();
            } else {
                _imp->dialog->reject();
            }
        }
        e->accept();
    } else if (key == Qt::Key_Down) {
        if (viewVisible) {
            int row = currentIndex.row() + 1;

            ///Handle circular selection
            if (row >= count) {
                row = 0;
            }

            QModelIndex index = _imp->listView->model()->index(row, 0);
            _imp->listView->setCurrentIndex(index);
        }
    } else if (key == Qt::Key_Up) {
        if (viewVisible) {
            int row = currentIndex.row() - 1;

            ///Handle circular selection
            if (row < 0) {
                row = count - 1;
            }

            QModelIndex index = _imp->listView->model()->index(row, 0);
            _imp->listView->setCurrentIndex(index);
        }
    } else if ( (key == Qt::Key_Enter) || (key == Qt::Key_Return) ) {
        _imp->listView->hide();
        if (_imp->model->rowCount() == 1) {
            setText( _imp->model->index(0).data().toString() );
            Q_EMIT itemCompletionChosen();
            if (_imp->quickExitEnabled) {
                NodeCreationDialog* isNodeDialog = dynamic_cast<NodeCreationDialog*>(_imp->dialog);
                if (isNodeDialog) {
                    isNodeDialog->finish(true);
                } else {
                    if (_imp->dialog) {
                        _imp->dialog->accept();
                    }
                }
            }
            e->accept();
        } else {
            const QItemSelection selection = _imp->listView->selectionModel()->selection();
            QModelIndexList indexes = selection.indexes();
            bool doDialogEnd = false;
            if (indexes.size() == 1) {
                setText( _imp->model->index( indexes[0].row() ).data().toString() );
                doDialogEnd = true;
            } else if ( indexes.isEmpty() ) {
                if (_imp->model->rowCount() > 1) {
                    doDialogEnd = true;
                    setText( _imp->model->index(0).data().toString() );
                }
            }
            if (doDialogEnd) {
                Q_EMIT itemCompletionChosen();
                if (_imp->quickExitEnabled) {
                    NodeCreationDialog* isNodeDialog = dynamic_cast<NodeCreationDialog*>(_imp->dialog);
                    if (isNodeDialog) {
                        isNodeDialog->finish(true);
                    } else {
                        if (_imp->dialog) {
                            _imp->dialog->accept();
                        }
                    }
                }
                e->accept();
            } else {
                QLineEdit::keyPressEvent(e);
            }
        }
    } else {
        QLineEdit::keyPressEvent(e);
    }
} // keyPressEvent

void
CompleterLineEdit::showCompleter()
{
    _imp->listView->setFixedWidth( width() );
    filterText( text() );
}

struct NodeCreationDialogPrivate
{
    QVBoxLayout* layout;
    CompleterLineEdit* textEdit;
    PluginsMap items;
    bool isFinished;

    NodeCreationDialogPrivate()
        : layout(NULL)
        , textEdit(NULL)
        , items( appPTR->getPluginsList() )
        , isFinished(false)
    {
    }
};

static int
getPluginWeight(const QString& pluginID,
                int majVersion)
{
    QSettings settings( QString::fromUtf8(NATRON_ORGANIZATION_NAME), QString::fromUtf8(NATRON_APPLICATION_NAME) );
    QString propName( pluginID + QString::number(majVersion) + QString::fromUtf8("_tab_weight") );
    int curValue = settings.value( propName, QVariant(0) ).toInt();

    return curValue;
}

static void
incrementPluginWeight(const QString& pluginID,
                      int majVersion)
{
    QSettings settings( QString::fromUtf8(NATRON_ORGANIZATION_NAME), QString::fromUtf8(NATRON_APPLICATION_NAME) );
    QString propName( pluginID + QString::number(majVersion) + QString::fromUtf8("_tab_weight") );
    int curValue = settings.value( propName, QVariant(0) ).toInt();

    ++curValue;
    settings.setValue( propName, QVariant(curValue) );
}

NodeCreationDialog::NodeCreationDialog(const QString& initialFilter,
                                       QWidget* parent)
    : QDialog(parent)
    , _imp( new NodeCreationDialogPrivate() )
{
    setWindowTitle( tr("Node Creation Tool") );
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setObjectName( QString::fromUtf8("nodeCreationDialog") );
    setAttribute(Qt::WA_DeleteOnClose, true);
    _imp->layout = new QVBoxLayout(this);
    _imp->layout->setContentsMargins(0, 0, 0, 0);


    CompleterLineEdit::PluginsNamesMap pluginsMap;
    QString initialFilterName;
    std::string stdInitialFilter = initialFilter.toStdString();
    int i = 0;
    for (PluginsMap::iterator it = _imp->items.begin(); it != _imp->items.end(); ++it) {
        if ( it->second.empty() ) {
            continue;
        }


        if (it->second.size() == 1) {
            std::pair<QString, QString> idNamePair;
            Plugin* p = ( *it->second.begin() );
            if ( !p->getIsUserCreatable() ) {
                continue;
            }

            idNamePair.second = p->generateUserFriendlyPluginID();

            int indexOfBracket = idNamePair.second.lastIndexOf( QString::fromUtf8("  [") );
            if (indexOfBracket != -1) {
                idNamePair.first = idNamePair.second.left(indexOfBracket);
            }

            int weight = getPluginWeight( p->getPluginID(), p->getMajorVersion() );
            pluginsMap.insert( std::make_pair(weight, idNamePair) );

            if (it->first == stdInitialFilter) {
                initialFilterName = idNamePair.first;
            }
            ++i;
        } else {
            QString bestMajorName;
            for (PluginVersionsOrdered::reverse_iterator it2 = it->second.rbegin(); it2 != it->second.rend(); ++it2) {
                if ( !(*it2)->getIsUserCreatable() ) {
                    continue;
                }
                std::pair<QString, QString> idNamePair;
                if ( it2 == it->second.rbegin() ) {
                    idNamePair.second = (*it2)->generateUserFriendlyPluginID();
                    bestMajorName = idNamePair.second;
                } else {
                    idNamePair.second = (*it2)->generateUserFriendlyPluginIDMajorEncoded();
                }


                int indexOfBracket = idNamePair.second.lastIndexOf( QString::fromUtf8("  [") );
                if (indexOfBracket != -1) {
                    idNamePair.first = idNamePair.second.left(indexOfBracket);
                }

                ++i;

                int weight = getPluginWeight( (*it2)->getPluginID(), (*it2)->getMajorVersion() );
                pluginsMap.insert( std::make_pair(weight, idNamePair) );
            }
            if (it->first == stdInitialFilter) {
                initialFilterName = bestMajorName;
            }
        }
    }

    _imp->textEdit = new CompleterLineEdit(pluginsMap, true, this);
    // SOLVFX: slim dark search field with a light rounded frame
    _imp->textEdit->setStyleSheet( QString::fromUtf8(
        "QLineEdit { background-color: #1e1e1e; color: #e8e8e8; border: 1px solid #a0a0a0; border-radius: 4px; "
        "padding: 3px 8px; selection-background-color: #5a5a5a; }") );
    _imp->textEdit->setMinimumWidth( TO_DPIX(320) );
    setStyleSheet( QString::fromUtf8("QDialog#nodeCreationDialog { background: transparent; }") );
    if ( !initialFilterName.isEmpty() ) {
        _imp->textEdit->setText(initialFilterName);
    }

    QPoint global = QCursor::pos();
    QSize sizeH = sizeHint();
    global.rx() -= sizeH.width() / 2;
    global.ry() -= sizeH.height() / 2;
    move( global.x(), global.y() );

    _imp->layout->addWidget(_imp->textEdit);
    _imp->textEdit->setFocus();
    _imp->textEdit->selectAll();
    QTimer::singleShot( 20, _imp->textEdit, SLOT(showCompleter()) );
}

NodeCreationDialog::~NodeCreationDialog()
{
}

QString
NodeCreationDialog::getNodeName(int *major) const
{
    QString name = _imp->textEdit->text();


    for (PluginsMap::iterator it = _imp->items.begin(); it != _imp->items.end(); ++it) {
        if (it->second.size() == 1) {
            if ( ( *it->second.begin() )->generateUserFriendlyPluginID() == name ) {
                Plugin* p = ( *it->second.begin() );
                *major = p->getMajorVersion();
                const QString& ret = p->getPluginID();
                incrementPluginWeight(ret, *major);

                return ret;
            }
        } else {
            for (PluginVersionsOrdered::reverse_iterator it2 = it->second.rbegin(); it2 != it->second.rend(); ++it2) {
                if ( it2 == it->second.rbegin() ) {
                    if ( (*it2)->generateUserFriendlyPluginID() == name ) {
                        *major = (*it2)->getMajorVersion();
                        const QString& ret = (*it2)->getPluginID();
                        incrementPluginWeight(ret, *major);

                        return ret;
                    }
                } else {
                    if ( (*it2)->generateUserFriendlyPluginIDMajorEncoded() == name ) {
                        *major = (*it2)->getMajorVersion();
                        const QString& ret = (*it2)->getPluginID();
                        incrementPluginWeight(ret, *major);

                        return ret;
                    }
                }
            }
        }
    }

    return QString();
}

void
NodeCreationDialog::keyPressEvent(QKeyEvent* e)
{
    if ( (e->key() == Qt::Key_Return) || (e->key() == Qt::Key_Enter) ) {
        finish(true);
    } else if (e->key() == Qt::Key_Escape) {
        finish(false);
    } else {
        QDialog::keyPressEvent(e);
    }
}

void
NodeCreationDialog::changeEvent(QEvent* e)
{
    if (e->type() == QEvent::ActivationChange) {
        if ( !isActiveWindow() && ( qApp->activeWindow() != _imp->textEdit->getView() ) ) {
            finish(false);

            return;
        }
    }
    QDialog::changeEvent(e);
}

void
NodeCreationDialog::finish(bool accepted)
{
    if (_imp->isFinished) {
        return;
    }
    _imp->isFinished = true;
    Q_EMIT dialogFinished(accepted);
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_NodeCreationDialog.cpp"
