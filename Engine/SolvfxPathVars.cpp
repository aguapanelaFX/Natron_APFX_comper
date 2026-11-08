/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

#include "SolvfxPathVars.h"

#include <QtCore/QDir>
#include <QtCore/QMutex>
#include <QtCore/QSettings>
#include <QtCore/QStringList>

#include "Engine/Project.h"

NATRON_NAMESPACE_ENTER

namespace SolvfxPathVars {

static QMutex g_mutex;
static std::map<std::string, std::string> g_session; // derived from fallbacks for this session

static std::string
clean(const std::string& p)
{
    QString s = QDir::fromNativeSeparators( QString::fromUtf8( p.c_str() ) );
    while ( (s.size() > 1) && s.endsWith( QLatin1Char('/') ) && !s.endsWith( QString::fromUtf8(":/") ) ) {
        s.chop(1);
    }

    return s.toStdString();
}

std::map<std::string, std::string>
userVars()
{
    std::map<std::string, std::string> ret;
    QSettings settings( QString::fromUtf8("AGUAPANELA FX"), QString::fromUtf8("SOLVFX") );
    settings.beginGroup( QString::fromUtf8("pathVars") );
    const QStringList keys = settings.childKeys();
    for (int i = 0; i < keys.size(); ++i) {
        const std::string v = settings.value(keys[i]).toString().toStdString();
        if ( !v.empty() ) {
            ret[keys[i].toStdString()] = clean(v);
        }
    }
    settings.endGroup();

    return ret;
}

void
setUserVars(const std::map<std::string, std::string>& vars)
{
    QSettings settings( QString::fromUtf8("AGUAPANELA FX"), QString::fromUtf8("SOLVFX") );
    settings.remove( QString::fromUtf8("pathVars") );
    settings.beginGroup( QString::fromUtf8("pathVars") );
    for (std::map<std::string, std::string>::const_iterator it = vars.begin(); it != vars.end(); ++it) {
        if ( !it->first.empty() && (it->first != "SCN") ) {
            settings.setValue( QString::fromUtf8( it->first.c_str() ), QString::fromUtf8( clean(it->second).c_str() ) );
        }
    }
    settings.endGroup();
    QMutexLocker l(&g_mutex);
    for (std::map<std::string, std::string>::const_iterator it = vars.begin(); it != vars.end(); ++it) {
        g_session.erase(it->first);
    }
}

std::map<std::string, std::string>
all(const Project* project)
{
    std::map<std::string, std::string> ret;
    {
        QMutexLocker l(&g_mutex);
        ret = g_session;
    }
    const std::map<std::string, std::string> u = userVars();
    for (std::map<std::string, std::string>::const_iterator it = u.begin(); it != u.end(); ++it) {
        ret[it->first] = it->second;
    }
    // $OUT defaults to $PRJ/renders
    if ( ( ret.find("OUT") == ret.end() ) && ( ret.find("PRJ") != ret.end() ) ) {
        ret["OUT"] = ret["PRJ"] + "/renders";
    }
    if (project) {
        const QString scn = project->getProjectPath();
        if ( !scn.isEmpty() ) {
            ret["SCN"] = clean( scn.toStdString() );
        }
    }

    return ret;
}

// $NAME at the start of str: returns the name and the position after it
static bool
leadingVar(const std::string& str, std::string* name, std::size_t* end)
{
    if ( str.size() < 2 || (str[0] != '$') ) {
        return false;
    }
    std::size_t i = 1;
    while ( i < str.size() && ( isalnum( (unsigned char)str[i] ) || (str[i] == '_') ) ) {
        ++i;
    }
    if (i == 1) {
        return false;
    }
    if ( (i < str.size()) && (str[i] != '/') && (str[i] != '\\') ) {
        return false;
    }
    *name = str.substr(1, i - 1);
    *end = i;

    return true;
}

bool
expand(std::string& str, const Project* project)
{
    std::string name;
    std::size_t end;
    if ( !leadingVar(str, &name, &end) ) {
        return false;
    }
    const std::map<std::string, std::string> vars = all(project);
    std::map<std::string, std::string>::const_iterator found = vars.find(name);
    if ( found == vars.end() ) {
        return false;
    }
    str = found->second + str.substr(end);

    return true;
}

bool
hasUnresolved(const std::string& str, const Project* project, std::string* nameOut)
{
    std::string name;
    std::size_t end;
    if ( !leadingVar(str, &name, &end) ) {
        return false;
    }
    const std::map<std::string, std::string> vars = all(project);
    if ( vars.find(name) != vars.end() ) {
        return false;
    }
    if (nameOut) {
        *nameOut = name;
    }

    return true;
}

bool
toVariable(std::string& str, const Project* project)
{
    if ( str.empty() || (str[0] == '$') || (str[0] == '[') ) {
        return false;
    }
    const std::string s = clean(str);
    const std::map<std::string, std::string> vars = all(project);
    std::string bestName, bestVal;
    for (std::map<std::string, std::string>::const_iterator it = vars.begin(); it != vars.end(); ++it) {
        const std::string& v = it->second;
        if ( v.empty() || ( v.size() >= s.size() ) || ( s.compare(0, v.size(), v) != 0 ) || (s[v.size()] != '/') ) {
            continue;
        }
        // longest wins; on equal length prefer the specific ones over the comp folder
        if ( ( v.size() > bestVal.size() ) || ( ( v.size() == bestVal.size() ) && (bestName == "SCN") ) ) {
            bestName = it->first;
            bestVal = v;
        }
    }
    if ( bestName.empty() ) {
        return false;
    }
    str = "$" + bestName + s.substr( bestVal.size() );

    return true;
}

bool
deriveFromFallback(const std::string& str, const std::string& absolute, const Project* project)
{
    std::string name;
    if ( absolute.empty() || !hasUnresolved(str, project, &name) ) {
        return false;
    }
    const std::string rest = clean( str.substr( name.size() + 1 ) );
    const std::string abs = clean(absolute);
    if ( rest.empty() || ( abs.size() <= rest.size() ) || ( abs.compare(abs.size() - rest.size(), rest.size(), rest) != 0 ) ) {
        return false;
    }
    const std::string base = abs.substr(0, abs.size() - rest.size());
    if ( base.empty() ) {
        return false;
    }
    QMutexLocker l(&g_mutex);
    g_session[name] = base;

    return true;
}
} // namespace SolvfxPathVars

NATRON_NAMESPACE_EXIT
