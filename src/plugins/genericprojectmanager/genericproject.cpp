/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "genericproject.h"

#include "genericbuildconfiguration.h"
#include "genericmakestep.h"
#include "genericprojectconstants.h"

#include <coreplugin/documentmanager.h>
#include <coreplugin/icontext.h>
#include <coreplugin/icore.h>
#include <cpptools/cpptoolsconstants.h>
#include <cpptools/cppmodelmanager.h>
#include <cpptools/projectinfo.h>
#include <cpptools/projectpartbuilder.h>
#include <extensionsystem/pluginmanager.h>
#include <projectexplorer/abi.h>
#include <projectexplorer/buildsteplist.h>
#include <projectexplorer/customexecutablerunconfiguration.h>
#include <projectexplorer/headerpath.h>
#include <projectexplorer/kitinformation.h>
#include <projectexplorer/kitmanager.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <qtsupport/baseqtversion.h>
#include <qtsupport/qtkitinformation.h>
#include <utils/algorithm.h>
#include <utils/fileutils.h>
#include <utils/qtcassert.h>

#include <QDir>
#include <QProcessEnvironment>

using namespace Core;
using namespace ProjectExplorer;

namespace GenericProjectManager {
namespace Internal {

////////////////////////////////////////////////////////////////////////////////////
//
// GenericProject
//
////////////////////////////////////////////////////////////////////////////////////

GenericProject::GenericProject(Manager *manager, const QString &fileName)
{
    setId(Constants::GENERICPROJECT_ID);
    setProjectManager(manager);
    setDocument(new GenericProjectFile(this, fileName, GenericProject::Everything));
    setRootProjectNode(new GenericProjectNode(this));
    setProjectContext(Context(GenericProjectManager::Constants::PROJECTCONTEXT));
    setProjectLanguages(Context(ProjectExplorer::Constants::LANG_CXX));

    QFileInfo fileInfo = projectFilePath().toFileInfo();
    QDir dir = fileInfo.dir();

    m_projectName      = fileInfo.completeBaseName();
    m_filesFileName    = QFileInfo(dir, m_projectName + QLatin1String(".files")).absoluteFilePath();
    m_includesFileName = QFileInfo(dir, m_projectName + QLatin1String(".includes")).absoluteFilePath();
    m_configFileName   = QFileInfo(dir, m_projectName + QLatin1String(".config")).absoluteFilePath();

    m_filesIDocument    = new GenericProjectFile(this, m_filesFileName, GenericProject::Files);
    m_includesIDocument = new GenericProjectFile(this, m_includesFileName, GenericProject::Configuration);
    m_configIDocument   = new GenericProjectFile(this, m_configFileName, GenericProject::Configuration);

    DocumentManager::addDocument(document());
    DocumentManager::addDocument(m_filesIDocument);
    DocumentManager::addDocument(m_includesIDocument);
    DocumentManager::addDocument(m_configIDocument);

    FileNode *projectFilesNode = new FileNode(Utils::FileName::fromString(m_filesFileName),
                                              ProjectFileType,
                                              /* generated = */ false);

    FileNode *projectIncludesNode = new FileNode(Utils::FileName::fromString(m_includesFileName),
                                                 ProjectFileType,
                                                 /* generated = */ false);

    FileNode *projectConfigNode = new FileNode(Utils::FileName::fromString(m_configFileName),
                                               ProjectFileType,
                                               /* generated = */ false);

    rootProjectNode()->addFileNodes(QList<FileNode *>() << projectFilesNode
                                    << projectIncludesNode << projectConfigNode);

    projectManager()->registerProject(this);
}

GenericProject::~GenericProject()
{
    m_codeModelFuture.cancel();
    projectManager()->unregisterProject(this);
}

QString GenericProject::filesFileName() const
{
    return m_filesFileName;
}

QString GenericProject::includesFileName() const
{
    return m_includesFileName;
}

QString GenericProject::configFileName() const
{
    return m_configFileName;
}

static QStringList readLines(const QString &absoluteFileName)
{
    QStringList lines;

    QFile file(absoluteFileName);
    if (file.open(QFile::ReadOnly)) {
        QTextStream stream(&file);

        forever {
            const QString line = stream.readLine();
            if (line.isNull())
                break;

            lines.append(line);
        }
    }

    return lines;
}

bool GenericProject::saveRawFileList(const QStringList &rawFileList)
{
    bool result = saveRawList(rawFileList, filesFileName());
    refresh(GenericProject::Files);
    return result;
}

bool GenericProject::saveRawList(const QStringList &rawList, const QString &fileName)
{
    FileChangeBlocker changeGuard(fileName);
    // Make sure we can open the file for writing
    Utils::FileSaver saver(fileName, QIODevice::Text);
    if (!saver.hasError()) {
        QTextStream stream(saver.file());
        foreach (const QString &filePath, rawList)
            stream << filePath << QLatin1Char('\n');
        saver.setResult(&stream);
    }
    bool result = saver.finalize(ICore::mainWindow());
    return result;
}

static void insertSorted(QStringList *list, const QString &value)
{
    int pos = Utils::indexOf(*list, [value](const QString &s) { return s > value; });
    list->insert(pos, value);
}

bool GenericProject::addFiles(const QStringList &filePaths)
{
    QStringList newList = m_rawFileList;

    QDir baseDir(projectDirectory().toString());
    foreach (const QString &filePath, filePaths)
        insertSorted(&newList, baseDir.relativeFilePath(filePath));

    QSet<QString> includes = projectIncludePaths().toSet();
    QSet<QString> toAdd;

    foreach (const QString &filePath, filePaths) {
        const QString directory = QFileInfo(filePath).absolutePath();
        if (!includes.contains(directory) && !toAdd.contains(directory))
            toAdd << directory;
    }

    const QDir dir(projectDirectory().toString());
    foreach (const QString &path, toAdd) {
        QString relative = dir.relativeFilePath(path);
        if (relative.isEmpty())
            relative = QLatin1Char('.');
        m_rawProjectIncludePaths.append(relative);
    }

    bool result = saveRawList(newList, filesFileName());
    result &= saveRawList(m_rawProjectIncludePaths, includesFileName());
    refresh(GenericProject::Everything);

    return result;
}

bool GenericProject::removeFiles(const QStringList &filePaths)
{
    QStringList newList = m_rawFileList;

    foreach (const QString &filePath, filePaths) {
        QHash<QString, QString>::iterator i = m_rawListEntries.find(filePath);
        if (i != m_rawListEntries.end())
            newList.removeOne(i.value());
    }

    return saveRawFileList(newList);
}

bool GenericProject::setFiles(const QStringList &filePaths)
{
    QStringList newList;
    QDir baseDir(projectDirectory().toString());
    foreach (const QString &filePath, filePaths)
        newList.append(baseDir.relativeFilePath(filePath));
    Utils::sort(newList);

    return saveRawFileList(newList);
}

bool GenericProject::renameFile(const QString &filePath, const QString &newFilePath)
{
    QStringList newList = m_rawFileList;

    QHash<QString, QString>::iterator i = m_rawListEntries.find(filePath);
    if (i != m_rawListEntries.end()) {
        int index = newList.indexOf(i.value());
        if (index != -1) {
            QDir baseDir(projectDirectory().toString());
            newList.removeAt(index);
            insertSorted(&newList, baseDir.relativeFilePath(newFilePath));
        }
    }

    return saveRawFileList(newList);
}

void GenericProject::parseProject(RefreshOptions options)
{
    if (options & Files) {
        m_rawListEntries.clear();
        m_rawFileList = readLines(filesFileName());
        m_files = processEntries(m_rawFileList, &m_rawListEntries);
    }

    if (options & Configuration) {
        m_rawProjectIncludePaths = readLines(includesFileName());
        m_projectIncludePaths = processEntries(m_rawProjectIncludePaths);

        // TODO: Possibly load some configuration from the project file
        //QSettings projectInfo(m_fileName, QSettings::IniFormat);
    }

    if (options & Files)
        emit fileListChanged();
}

void GenericProject::refresh(RefreshOptions options)
{
    parseProject(options);

    if (options & Files) {
        QList<FileNode *> fileNodes = Utils::transform(files(), [](const QString &f) {
            FileType fileType = SourceType; // ### FIXME
            if (f.endsWith(QLatin1String(".qrc")))
                fileType = ResourceType;
            return new FileNode(Utils::FileName::fromString(f), fileType, false);
        });
        rootProjectNode()->buildTree(fileNodes);
    }

    refreshCppCodeModel();
}

/**
 * Expands environment variables in the given \a string when they are written
 * like $$(VARIABLE).
 */
static void expandEnvironmentVariables(const QProcessEnvironment &env, QString &string)
{
    static QRegExp candidate(QLatin1String("\\$\\$\\((.+)\\)"));

    int index = candidate.indexIn(string);
    while (index != -1) {
        const QString value = env.value(candidate.cap(1));

        string.replace(index, candidate.matchedLength(), value);
        index += value.length();

        index = candidate.indexIn(string, index);
    }
}

/**
 * Expands environment variables and converts the path from relative to the
 * project to an absolute path.
 *
 * The \a map variable is an optional argument that will map the returned
 * absolute paths back to their original \a entries.
 */
QStringList GenericProject::processEntries(const QStringList &paths,
                                           QHash<QString, QString> *map) const
{
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QDir projectDir(projectDirectory().toString());

    QFileInfo fileInfo;
    QStringList absolutePaths;
    foreach (const QString &path, paths) {
        QString trimmedPath = path.trimmed();
        if (trimmedPath.isEmpty())
            continue;

        expandEnvironmentVariables(env, trimmedPath);

        trimmedPath = Utils::FileName::fromUserInput(trimmedPath).toString();

        fileInfo.setFile(projectDir, trimmedPath);
        if (fileInfo.exists()) {
            const QString absPath = fileInfo.absoluteFilePath();
            absolutePaths.append(absPath);
            if (map)
                map->insert(absPath, trimmedPath);
        }
    }
    absolutePaths.removeDuplicates();
    return absolutePaths;
}

void GenericProject::refreshCppCodeModel()
{
    CppTools::CppModelManager *modelManager = CppTools::CppModelManager::instance();

    m_codeModelFuture.cancel();

    CppTools::ProjectInfo pInfo(this);
    CppTools::ProjectPartBuilder ppBuilder(pInfo);

    CppTools::ProjectPart::QtVersion activeQtVersion = CppTools::ProjectPart::NoQt;
    if (QtSupport::BaseQtVersion *qtVersion =
            QtSupport::QtKitInformation::qtVersion(activeTarget()->kit())) {
        if (qtVersion->qtVersion() < QtSupport::QtVersionNumber(5,0,0))
            activeQtVersion = CppTools::ProjectPart::Qt4;
        else
            activeQtVersion = CppTools::ProjectPart::Qt5;
    }

    ppBuilder.setQtVersion(activeQtVersion);
    ppBuilder.setIncludePaths(projectIncludePaths());
    ppBuilder.setConfigFileName(configFileName());
    ppBuilder.setCxxFlags(QStringList() << QLatin1String("-std=c++11"));

    const QList<Id> languages = ppBuilder.createProjectPartsForFiles(files());
    foreach (Id language, languages)
        setProjectLanguage(language, true);

    pInfo.finish();
    m_codeModelFuture = modelManager->updateProjectInfo(pInfo);
}

QStringList GenericProject::projectIncludePaths() const
{
    return m_projectIncludePaths;
}

QStringList GenericProject::files() const
{
    return m_files;
}

QString GenericProject::displayName() const
{
    return m_projectName;
}

Manager *GenericProject::projectManager() const
{
    return static_cast<Manager *>(Project::projectManager());
}

QStringList GenericProject::files(FilesMode fileMode) const
{
    Q_UNUSED(fileMode);
    return m_files;
}

QStringList GenericProject::buildTargets() const
{
    QStringList targets;
    targets.append(QLatin1String("all"));
    targets.append(QLatin1String("clean"));
    return targets;
}

Project::RestoreResult GenericProject::fromMap(const QVariantMap &map, QString *errorMessage)
{
    RestoreResult result = Project::fromMap(map, errorMessage);
    if (result != RestoreResult::Ok)
        return result;

    Kit *defaultKit = KitManager::defaultKit();
    if (!activeTarget() && defaultKit)
        addTarget(createTarget(defaultKit));

    // Sanity check: We need both a buildconfiguration and a runconfiguration!
    QList<Target *> targetList = targets();
    if (targetList.isEmpty())
        return RestoreResult::Error;

    foreach (Target *t, targetList) {
        if (!t->activeBuildConfiguration()) {
            removeTarget(t);
            continue;
        }
        if (!t->activeRunConfiguration())
            t->addRunConfiguration(new CustomExecutableRunConfiguration(t));
    }

    refresh(Everything);
    return RestoreResult::Ok;
}

////////////////////////////////////////////////////////////////////////////////////
//
// GenericProjectFile
//
////////////////////////////////////////////////////////////////////////////////////

GenericProjectFile::GenericProjectFile(GenericProject *parent, QString fileName,
                                       GenericProject::RefreshOptions options) :
      m_project(parent),
      m_options(options)
{
    setId("Generic.ProjectFile");
    setMimeType(QLatin1String(Constants::GENERICMIMETYPE));
    setFilePath(Utils::FileName::fromString(fileName));
}

IDocument::ReloadBehavior GenericProjectFile::reloadBehavior(ChangeTrigger state, ChangeType type) const
{
    Q_UNUSED(state);
    Q_UNUSED(type);
    return BehaviorSilent;
}

bool GenericProjectFile::reload(QString *errorString, ReloadFlag flag, ChangeType type)
{
    Q_UNUSED(errorString);
    Q_UNUSED(flag);
    if (type == TypePermissions)
        return true;
    m_project->refresh(m_options);
    return true;
}

} // namespace Internal
} // namespace GenericProjectManager
