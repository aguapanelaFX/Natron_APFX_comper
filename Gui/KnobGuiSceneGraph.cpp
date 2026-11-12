/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

#include <Python.h>

#include "KnobGuiSceneGraph.h"

#include <sstream>
#include <vector>

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

namespace {
enum Col { eColPath = 0, eColLoad, eColActive, eColVisible, eColType, eColKind, eColPurpose, eColCount };

const QColor kArc(0xe8, 0xa3, 0x3d);
const QColor kInstance(0x8f, 0xd3, 0xff);
const QColor kPrototype(0x7d, 0x86, 0xe0);

std::vector<std::string>
split(const std::string& s, char c)
{
    std::vector<std::string> r;
    std::string cur;
    for (char ch : s) {
        if (ch == c) {
            r.push_back(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    r.push_back(cur);

    return r;
}

int
filterOf(const std::string& type)
{
    if (type.find("Light") != std::string::npos) {
        return 0;
    }
    if (type == "Camera") {
        return 1;
    }
    if ( (type == "Material") || (type == "Shader") || (type == "NodeGraph") ) {
        return 2;
    }
    if ( (type == "Mesh") || (type == "GeomSubset") || (type == "Points") || (type == "BasisCurves") || (type == "Cube") ||
         (type == "Sphere") || (type == "Cylinder") || (type == "Cone") || (type == "Capsule") || (type == "Plane") ) {
        return 3;
    }

    return -1;
}
} // anon

KnobGuiSceneGraph::KnobGuiSceneGraph(KnobIPtr knob, KnobGuiContainerI *container)
    : KnobGui(knob, container)
{
    _knob = std::dynamic_pointer_cast<KnobSceneGraph>(knob);
    for (int i = 0; i < 4; ++i) {
        _filter[i] = 0;
    }
}

KnobGuiSceneGraph::~KnobGuiSceneGraph()
{
}

KnobIPtr
KnobGuiSceneGraph::getKnob() const
{
    return _knob.lock();
}

void
KnobGuiSceneGraph::createWidget(QHBoxLayout* layout)
{
    _w = new QWidget( layout->parentWidget() );
    _w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    QVBoxLayout* v = new QVBoxLayout(_w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    _search = new QLineEdit(_w);
    _search->setPlaceholderText( QString::fromUtf8("search") );
    _search->setClearButtonEnabled(true);
    v->addWidget(_search);

    QHBoxLayout* fl = new QHBoxLayout();
    const char* names[4] = { "lights", "cameras", "materials", "meshes" };
    for (int i = 0; i < 4; ++i) {
        _filter[i] = new QToolButton(_w);
        _filter[i]->setText( QString::fromUtf8(names[i]) );
        _filter[i]->setCheckable(true);
        _filter[i]->setToolTip( QString::fromUtf8("Show only these prims (and their parents). Several can be on.") );
        fl->addWidget(_filter[i]);
        QObject::connect( _filter[i], &QToolButton::toggled, _w, [this](bool) { applyFilter(); } );
    }
    fl->addStretch(1);
    fl->addWidget( new QLabel(QString::fromUtf8("graph depth"), _w) );
    _depth = new QSpinBox(_w);
    _depth->setRange(1, 64);
    _depth->setValue(10);
    _depth->setToolTip( QString::fromUtf8("How many levels are shown.") );
    fl->addWidget(_depth);
    v->addLayout(fl);

    QHBoxLayout* lg = new QHBoxLayout();
    auto legend = [&](const QColor& c, const char* text) {
        QLabel* l = new QLabel( QString::fromUtf8("<span style='color:%1'>&#9632;</span> %2").arg( c.name() ).arg( QString::fromUtf8(text) ), _w );
        lg->addWidget(l);
    };
    legend(kArc, "has arc");
    legend(kInstance, "instance");
    legend(kPrototype, "prototype");
    legend(QColor(0xc8, 0xc8, 0xc8), "normal");
    lg->addStretch(1);
    v->addLayout(lg);
    QLabel* note = new QLabel( QString::fromUtf8("<i>italic: prims with only 'over' specifiers</i>"), _w );
    v->addWidget(note);

    _tree = new QTreeWidget(_w);
    _tree->setColumnCount(eColCount);
    QStringList hdr;
    hdr << QString::fromUtf8("path") << QString::fromUtf8("load") << QString::fromUtf8("active") << QString::fromUtf8("visible")
        << QString::fromUtf8("type") << QString::fromUtf8("kind") << QString::fromUtf8("purpose");
    _tree->setHeaderLabels(hdr);
    _tree->headerItem()->setToolTip( eColLoad, QString::fromUtf8("Load the payload (the geometry it points to).") );
    _tree->headerItem()->setToolTip( eColActive, QString::fromUtf8("Active: off removes the prim and everything under it.") );
    _tree->headerItem()->setToolTip( eColVisible, QString::fromUtf8("Visible: off hides it (and its children).") );
    _tree->setMinimumHeight(420);
    _tree->setUniformRowHeights(true);
    _tree->header()->setStretchLastSection(false);
    _tree->header()->setSectionResizeMode(eColPath, QHeaderView::Stretch);
    for (int c = eColLoad; c < eColCount; ++c) {
        _tree->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    }
    v->addWidget(_tree, 1);
    _message = new QLabel(_w);
    _message->setWordWrap(true);
    v->addWidget(_message);

    QHBoxLayout* bl = new QHBoxLayout();
    const char* bn[3] = { "reset payloads", "reset active / inactive", "reset visibility" };
    const char flags[3] = { 'u', 'd', 'h' };
    for (int i = 0; i < 3; ++i) {
        QPushButton* b = new QPushButton( QString::fromUtf8(bn[i]), _w );
        const char f = flags[i];
        QObject::connect( b, &QPushButton::clicked, _w, [this, f]() { resetFlag(f); } );
        bl->addWidget(b);
    }
    bl->addStretch(1);
    v->addLayout(bl);

    QObject::connect( _search, &QLineEdit::textChanged, _w, [this](const QString&) { applyFilter(); } );
    QObject::connect( _depth, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged), _w, [this](int) { applyFilter(); } );
    QObject::connect( _tree, &QTreeWidget::itemChanged, _w, [this](QTreeWidgetItem*, int) {
        if (!_building) {
            writeEdits();
        }
    } );
    QObject::connect( _tree, &QTreeWidget::itemExpanded, _w, [this](QTreeWidgetItem* it) {
        _expanded.insert( it->data(eColPath, Qt::UserRole).toString().toStdString() );
    } );
    QObject::connect( _tree, &QTreeWidget::itemCollapsed, _w, [this](QTreeWidgetItem* it) {
        _expanded.erase( it->data(eColPath, Qt::UserRole).toString().toStdString() );
    } );

    _timer = new QTimer(_w);
    _timer->setInterval(300);
    QObject::connect( _timer, &QTimer::timeout, _w, [this]() { poll(); } );
    _timer->start();
    layout->addWidget(_w, 1);
    poll();
}

void
KnobGuiSceneGraph::poll()
{
    KnobSceneGraphPtr k = _knob.lock();
    if ( !k || !_tree || !_w->isVisible() ) {
        return;
    }
    int rev = 0;
    std::string msg;
    k->getTree(&rev, &msg);
    if (rev != _rev) {
        rebuild();
    }
}

void
KnobGuiSceneGraph::updateGUI(int)
{
    // edits changed (undo, load): show them
    if (_tree) {
        rebuild();
    }
}

void
KnobGuiSceneGraph::rebuild()
{
    KnobSceneGraphPtr k = _knob.lock();
    if (!k || !_tree) {
        return;
    }
    std::string msg;
    const std::string tree = k->getTree(&_rev, &msg);
    // current edits
    _edits.clear();
    for ( const std::string& line : split(k->getValue(), '\n') ) {
        if ( (line.size() > 2) && (line[1] == ' ') ) {
            _edits[line[0]].insert( line.substr(2) );
        }
    }
    _message->setText( QString::fromUtf8( msg.c_str() ) );
    _message->setVisible( !msg.empty() );
    _building = true;
    _tree->clear();
    std::map<std::string, QTreeWidgetItem*> items;
    const bool firstFill = _expanded.empty();
    for ( const std::string& line : split(tree, '\n') ) {
        std::vector<std::string> f = split(line, '\t');
        if ( (f.size() < 5) || f[0].empty() ) {
            continue;
        }
        const std::string& path = f[0];
        const std::string& flags = f[4];
        const size_t slash = path.rfind('/');
        const std::string parent = (slash == 0 || slash == std::string::npos) ? std::string() : path.substr(0, slash);
        QTreeWidgetItem* pi = 0;
        std::map<std::string, QTreeWidgetItem*>::const_iterator pit = items.find(parent);
        if ( pit != items.end() ) {
            pi = pit->second;
        }
        QTreeWidgetItem* it = pi ? new QTreeWidgetItem(pi) : new QTreeWidgetItem(_tree);
        items[path] = it;
        it->setText( eColPath, QString::fromUtf8( path.substr(slash + 1).c_str() ) );
        it->setData( eColPath, Qt::UserRole, QString::fromUtf8( path.c_str() ) );
        it->setData( eColType, Qt::UserRole, filterOf(f[1]) );
        it->setToolTip( eColPath, QString::fromUtf8( path.c_str() ) );
        it->setText( eColType, QString::fromUtf8( f[1].c_str() ) );
        it->setText( eColKind, QString::fromUtf8( f[2].c_str() ) );
        it->setText( eColPurpose, QString::fromUtf8( f[3].c_str() ) );
        QColor col;
        if (flags.find('i') != std::string::npos) {
            col = kInstance;
        } else if (flags.find('p') != std::string::npos) {
            col = kPrototype;
        } else if (flags.find('a') != std::string::npos) {
            col = kArc;
        }
        if ( col.isValid() ) {
            for (int c = 0; c < eColCount; ++c) {
                it->setForeground(c, col);
            }
        }
        if (flags.find('o') != std::string::npos) {
            QFont fo = it->font(eColPath);
            fo.setItalic(true);
            it->setFont(eColPath, fo);
        }
        Qt::ItemFlags itf = it->flags() | Qt::ItemIsUserCheckable;
        it->setFlags(itf);
        if (flags.find('P') != std::string::npos) {
            it->setCheckState(eColLoad, _edits['u'].count(path) ? Qt::Unchecked : Qt::Checked);
        }
        it->setCheckState(eColActive, _edits['d'].count(path) ? Qt::Unchecked : Qt::Checked);
        if ( (f[1] != "Material") && (f[1] != "Shader") && (f[1] != "Scope") && (f[1] != "GeomSubset") && (f[1] != "NodeGraph") ) {
            it->setCheckState(eColVisible, _edits['h'].count(path) ? Qt::Unchecked : Qt::Checked);
        }
        if ( firstFill ? (path.find('/', 1) == std::string::npos) : (_expanded.count(path) != 0) ) {
            it->setExpanded(true);
            _expanded.insert(path);
        }
    }
    _building = false;
    applyFilter();
}

void
KnobGuiSceneGraph::applyFilter()
{
    if (!_tree) {
        return;
    }
    const QString text = _search->text().trimmed();
    bool anyFilter = false;
    bool on[4];
    for (int i = 0; i < 4; ++i) {
        on[i] = _filter[i]->isChecked();
        anyFilter = anyFilter || on[i];
    }
    const int maxDepth = _depth->value();
    // depth first: a row shows when it matches, or a child of it shows
    std::function<bool(QTreeWidgetItem*, int)> visit = [&](QTreeWidgetItem* it, int depth) -> bool {
        bool childShown = false;
        for (int i = 0; i < it->childCount(); ++i) {
            childShown = visit(it->child(i), depth + 1) || childShown;
        }
        bool match = true;
        if ( !text.isEmpty() ) {
            match = it->text(eColPath).contains(text, Qt::CaseInsensitive) || it->text(eColType).contains(text, Qt::CaseInsensitive);
        }
        if (anyFilter) {
            const int fo = it->data(eColType, Qt::UserRole).toInt();
            match = match && (fo >= 0) && on[fo];
        }
        const bool show = (depth <= maxDepth) && (match || childShown);
        it->setHidden(!show);

        return show;
    };
    for (int i = 0; i < _tree->topLevelItemCount(); ++i) {
        visit(_tree->topLevelItem(i), 1);
    }
}

void
KnobGuiSceneGraph::writeEdits()
{
    KnobSceneGraphPtr k = _knob.lock();
    if (!k) {
        return;
    }
    std::map<char, std::set<std::string> > e;
    std::function<void(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* it) {
        const std::string path = it->data(eColPath, Qt::UserRole).toString().toStdString();
        if ( it->data(eColLoad, Qt::CheckStateRole).isValid() && (it->checkState(eColLoad) == Qt::Unchecked) ) {
            e['u'].insert(path);
        }
        if ( it->data(eColActive, Qt::CheckStateRole).isValid() && (it->checkState(eColActive) == Qt::Unchecked) ) {
            e['d'].insert(path);
        }
        if ( it->data(eColVisible, Qt::CheckStateRole).isValid() && (it->checkState(eColVisible) == Qt::Unchecked) ) {
            e['h'].insert(path);
        }
        for (int i = 0; i < it->childCount(); ++i) {
            visit( it->child(i) );
        }
    };
    for (int i = 0; i < _tree->topLevelItemCount(); ++i) {
        visit( _tree->topLevelItem(i) );
    }
    // keep the edits of prims not in the tree now (under an inactive / unloaded parent)
    for (std::map<char, std::set<std::string> >::const_iterator it = _edits.begin(); it != _edits.end(); ++it) {
        for (const std::string& p : it->second) {
            bool inTree = false;
            for (int i = 0; i < _tree->topLevelItemCount() && !inTree; ++i) {
                std::function<bool(QTreeWidgetItem*)> find = [&](QTreeWidgetItem* x) -> bool {
                    if (x->data(eColPath, Qt::UserRole).toString().toStdString() == p) {
                        return true;
                    }
                    for (int c = 0; c < x->childCount(); ++c) {
                        if ( find( x->child(c) ) ) {
                            return true;
                        }
                    }

                    return false;
                };
                inTree = find( _tree->topLevelItem(i) );
            }
            if (!inTree) {
                e[it->first].insert(p);
            }
        }
    }
    std::string out;
    const char order[3] = { 'u', 'd', 'h' };
    for (int i = 0; i < 3; ++i) {
        for (const std::string& p : e[order[i]]) {
            out += std::string(1, order[i]) + " " + p + "\n";
        }
    }
    _edits = e;
    k->setValue(out, ViewSpec::all(), 0, eValueChangedReasonUserEdited, 0);
}

void
KnobGuiSceneGraph::resetFlag(char f)
{
    KnobSceneGraphPtr k = _knob.lock();
    if (!k) {
        return;
    }
    std::string out;
    for ( const std::string& line : split(k->getValue(), '\n') ) {
        if ( (line.size() > 2) && (line[0] != f) ) {
            out += line + "\n";
        }
    }
    k->setValue(out, ViewSpec::all(), 0, eValueChangedReasonUserEdited, 0);
}

void
KnobGuiSceneGraph::removeSpecificGui()
{
    if (_w) {
        _w->deleteLater();
        _w = 0;
        _tree = 0;
    }
}

void
KnobGuiSceneGraph::_hide()
{
    if (_w) {
        _w->hide();
    }
}

void
KnobGuiSceneGraph::_show()
{
    if (_w) {
        _w->show();
        poll();
    }
}

NATRON_NAMESPACE_EXIT
