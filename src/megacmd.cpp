/**
 * @file src/megacmd.cpp
 * @brief MEGAcmd: Interactive CLI and service application
 *
 * (c) 2013 by Mega Limited, Auckland, New Zealand
 *
 * This file is part of the MEGAcmd.
 *
 * MEGAcmd is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#include "megacmd.h"

#include "megacmdsandbox.h"
#include "megacmdexecuter.h"
#include "megacmdutils.h"
#include "configurationmanager.h"
#include "megacmdlogger.h"
#include "comunicationsmanager.h"
#include "listeners.h"

#include "megacmdplatform.h"
#include "megacmdversion.h"

#define USE_VARARGS
#define PREFER_STDARG

#include <iomanip>
#include <string>

#ifndef _WIN32
#include "signal.h"
#include <sys/wait.h>
#else
#include <taskschd.h>
#include <comutil.h>
#include <comdef.h>
#include <sddl.h>
#include <fcntl.h>
#include <io.h>
#define strdup _strdup  // avoid warning
#endif

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#if !defined (PARAMS)
#  if defined (__STDC__) || defined (__GNUC__) || defined (__cplusplus)
#    define PARAMS(protos) protos
#  else
#    define PARAMS(protos) ()
#  endif
#endif


#define SSTR( x ) static_cast< const std::ostringstream & >( \
        ( std::ostringstream() << std::dec << x ) ).str()


#ifndef ERRNO
#ifdef _WIN32
#include <windows.h>
#define ERRNO WSAGetLastError()
#else
#define ERRNO errno
#endif
#endif

#ifdef _WIN32
#ifdef USE_PORT_COMMS
#include "comunicationsmanagerportsockets.h"
#define COMUNICATIONMANAGER ComunicationsManagerPortSockets
#else
#include "comunicationsmanagernamedpipes.h"
#define COMUNICATIONMANAGER ComunicationsManagerNamedPipes
#endif
#else
#include "comunicationsmanagerfilesockets.h"
#define COMUNICATIONMANAGER ComunicationsManagerFileSockets
#include <signal.h>
#endif

using namespace mega;

namespace megacmd {

typedef char *completionfunction_t PARAMS((const char *, int));

MegaCmdExecuter *cmdexecuter;
MegaCmdSandbox *sandboxCMD;

MegaSemaphore semaphoreClients; //to limit max parallel petitions

MegaApi *api;

//api objects for folderlinks
std::queue<MegaApi *> apiFolders;
std::vector<MegaApi *> occupiedapiFolders;
MegaSemaphore semaphoreapiFolders;
std::mutex mutexapiFolders;

MegaCMDLogger *loggerCMD;

std::mutex mutexEndedPetitionThreads;
std::vector<MegaThread *> petitionThreads;
std::vector<MegaThread *> endedPetitionThreads;
MegaThread *threadRetryConnections;

//Comunications Manager
ComunicationsManager * cm;

// global listener
MegaCmdGlobalListener* megaCmdGlobalListener;

MegaCmdMegaListener* megaCmdMegaListener;

bool loginInAtStartup = false;

string validGlobalParameters[] = {"v", "help"};

string alocalremotefolderpatterncommands [] = {"sync"};
vector<string> localremotefolderpatterncommands(alocalremotefolderpatterncommands, alocalremotefolderpatterncommands + sizeof alocalremotefolderpatterncommands / sizeof alocalremotefolderpatterncommands[0]);

string aremotepatterncommands[] = {"export", "attr"};
vector<string> remotepatterncommands(aremotepatterncommands, aremotepatterncommands + sizeof aremotepatterncommands / sizeof aremotepatterncommands[0]);

string aremotefolderspatterncommands[] = {"cd", "share"};
vector<string> remotefolderspatterncommands(aremotefolderspatterncommands, aremotefolderspatterncommands + sizeof aremotefolderspatterncommands / sizeof aremotefolderspatterncommands[0]);

string amultipleremotepatterncommands[] = {"ls", "tree", "mkdir", "rm", "du", "find", "mv", "deleteversions", "cat", "mediainfo"
#ifdef HAVE_LIBUV
                                           , "webdav", "ftp"
#endif
                                          };
vector<string> multipleremotepatterncommands(amultipleremotepatterncommands, amultipleremotepatterncommands + sizeof amultipleremotepatterncommands / sizeof amultipleremotepatterncommands[0]);

string aremoteremotepatterncommands[] = {"cp"};
vector<string> remoteremotepatterncommands(aremoteremotepatterncommands, aremoteremotepatterncommands + sizeof aremoteremotepatterncommands / sizeof aremoteremotepatterncommands[0]);

string aremotelocalpatterncommands[] = {"get", "thumbnail", "preview"};
vector<string> remotelocalpatterncommands(aremotelocalpatterncommands, aremotelocalpatterncommands + sizeof aremotelocalpatterncommands / sizeof aremotelocalpatterncommands[0]);

string alocalfolderpatterncommands [] = {"lcd"};
vector<string> localfolderpatterncommands(alocalfolderpatterncommands, alocalfolderpatterncommands + sizeof alocalfolderpatterncommands / sizeof alocalfolderpatterncommands[0]);

string aemailpatterncommands [] = {"invite", "signup", "ipc", "users"};
vector<string> emailpatterncommands(aemailpatterncommands, aemailpatterncommands + sizeof aemailpatterncommands / sizeof aemailpatterncommands[0]);

string avalidCommands [] = { "login", "signup", "confirm", "session", "mount", "ls", "cd", "log", "debug", "pwd", "lcd", "lpwd", "import", "masterkey",
                             "put", "get", "attr", "userattr", "mkdir", "rm", "du", "mv", "cp", "sync", "export", "share", "invite", "ipc", "df",
                             "showpcr", "users", "speedlimit", "killsession", "whoami", "help", "passwd", "reload", "logout", "version", "quit",
                             "thumbnail", "preview", "find", "completion", "clear", "https", "transfers", "exclude", "exit", "errorcode", "graphics",
                             "cancel", "confirmcancel", "cat", "tree", "psa"
                             , "mediainfo"
#ifdef HAVE_LIBUV
                             , "webdav", "ftp"
#endif
#ifdef ENABLE_BACKUPS
                             , "backup"
#endif
                             , "deleteversions"
#if defined(_WIN32) && defined(NO_READLINE)
                             , "autocomplete", "codepage"
#elif defined(_WIN32)
                             , "unicode"
#else
                             , "permissions"
#endif
#if defined(_WIN32) || defined(__APPLE__)
                             , "update"
#endif
                           };
vector<string> validCommands(avalidCommands, avalidCommands + sizeof avalidCommands / sizeof avalidCommands[0]);

// password change-related state information
string oldpasswd;
string newpasswd;

bool doExit = false;
bool consoleFailed = false;
bool alreadyCheckingForUpdates = false;
bool stopcheckingforUpdaters = false;

string dynamicprompt = "MEGA CMD> ";

static prompttype prompt = COMMAND;

// local console
Console* console;

std::mutex mutexHistory;

map<unsigned long long, string> threadline;

char ** mcmdMainArgv;
int mcmdMainArgc;

void printWelcomeMsg();

string getCurrentThreadLine()
{
    uint64_t currentThread = MegaThread::currentThreadId();
    if (threadline.find(currentThread) == threadline.end())
    { // not found thread
        return string();
    }
    else
    {
        return threadline[currentThread];
    }
}

void setCurrentThreadLine(string s)
{
    threadline[MegaThread::currentThreadId()] = s;
}

void setCurrentThreadLine(const vector<string>& vec)
{
   setCurrentThreadLine(joinStrings(vec));
}

void sigint_handler(int signum)
{
    LOG_verbose << "Received signal: " << signum;
    if (loginInAtStartup)
    {
        exit(-2);
    }

    LOG_debug << "Exiting due to SIGINT";

    stopcheckingforUpdaters = true;
    doExit = true;
}

#ifdef _WIN32
BOOL __stdcall CtrlHandler( DWORD fdwCtrlType )
{
  LOG_verbose << "Reached CtrlHandler: " << fdwCtrlType;

  switch( fdwCtrlType )
  {
    // Handle the CTRL-C signal.
    case CTRL_C_EVENT:
       sigint_handler((int)fdwCtrlType);
      return( TRUE );

    default:
      return FALSE;
  }
}
#endif

prompttype getprompt()
{
    return prompt;
}

void setprompt(prompttype p, string arg)
{
    prompt = p;

    if (p == COMMAND)
    {
        console->setecho(true);
    }
    else
    {
        if (arg.size())
        {
            OUTSTREAM << arg << flush;
        }
        else
        {
            OUTSTREAM << prompts[p] << flush;
        }

        console->setecho(false);
    }
}

void changeprompt(const char *newprompt)
{
    dynamicprompt = newprompt;
    string s = "prompt:";
    s+=dynamicprompt;
    cm->informStateListeners(s);
}

void informStateListener(string message, int clientID)
{
    string s;
    if (message.size())
    {
        s += "message:";
        s+=message;
    }
    cm->informStateListenerByClientId(s, clientID);
}

void broadcastMessage(string message)
{
    string s;
    if (message.size())
    {
        s += "message:";
        s+=message;
    }
    cm->informStateListeners(s);
}

void informTransferUpdate(MegaTransfer *transfer, int clientID)
{
    informProgressUpdate(transfer->getTransferredBytes(),transfer->getTotalBytes(), clientID);
}

void informStateListenerByClientId(int clientID, string s)
{
    cm->informStateListenerByClientId(s, clientID);
}

void informProgressUpdate(long long transferred, long long total, int clientID, string title)
{
    string s = "progress:";
    s+=SSTR(transferred);
    s+=":";
    s+=SSTR(total);

    if (title.size())
    {
        s+=":";
        s+=title;
    }

    informStateListenerByClientId(clientID, s);
}

void insertValidParamsPerCommand(set<string> *validParams, string thecommand, set<string> *validOptValues = NULL)
{
    if (!validOptValues)
    {
        validOptValues = validParams;
    }

    validOptValues->insert("client-width");


    if ("ls" == thecommand)
    {
        validParams->insert("R");
        validParams->insert("r");
        validParams->insert("l");
        validParams->insert("a");
        validParams->insert("h");
        validParams->insert("show-handles");
        validParams->insert("versions");
        validOptValues->insert("time-format");
        validParams->insert("tree");
#ifdef USE_PCRE
        validParams->insert("use-pcre");
#endif
    }
    else if ("passwd" == thecommand)
    {
        validParams->insert("f");
        validOptValues->insert("auth-code");
    }
    else if ("du" == thecommand)
    {
        validParams->insert("h");
        validParams->insert("versions");
        validOptValues->insert("path-display-size");
#ifdef USE_PCRE
        validParams->insert("use-pcre");
#endif
    }
    else if ("help" == thecommand)
    {
        validParams->insert("f");
        validParams->insert("non-interactive");
        validParams->insert("upgrade");
#ifdef _WIN32
        validParams->insert("unicode");
#endif
    }
    else if ("version" == thecommand)
    {
        validParams->insert("l");
        validParams->insert("c");
    }
    else if ("rm" == thecommand)
    {
        validParams->insert("r");
        validParams->insert("f");
#ifdef USE_PCRE
        validParams->insert("use-pcre");
#endif
    }
    else if ("mv" == thecommand)
    {
#ifdef USE_PCRE
        validParams->insert("use-pcre");
#endif
    }
    else if ("cp" == thecommand)
    {
#ifdef USE_PCRE
        validParams->insert("use-pcre");
#endif
    }
    else if ("speedlimit" == thecommand)
    {
        validParams->insert("u");
        validParams->insert("d");
        validParams->insert("h");
    }
    else if ("whoami" == thecommand)
    {
        validParams->insert("l");
    }
    else if ("df" == thecommand)
    {
        validParams->insert("h");
    }
    else if ("mediainfo" == thecommand)
    {
        validOptValues->insert("path-display-size");
    }
    else if ("log" == thecommand)
    {
        validParams->insert("c");
        validParams->insert("s");
    }
#ifndef _WIN32
    else if ("permissions" == thecommand)
    {
        validParams->insert("s");
        validParams->insert("files");
        validParams->insert("folders");
    }
#endif
    else if ("deleteversions" == thecommand)
    {
        validParams->insert("all");
        validParams->insert("f");
#ifdef USE_PCRE
        validParams->insert("use-pcre");
#endif
    }
    else if ("exclude" == thecommand)
    {
        validParams->insert("a");
        validParams->insert("d");
        validParams->insert("restart-syncs");
    }
#ifdef HAVE_LIBUV
    else if ("webdav" == thecommand)
    {
        validParams->insert("d");
        validParams->insert("all");
        validParams->insert("tls");
        validParams->insert("public");
        validOptValues->insert("port");
        validOptValues->insert("certificate");
        validOptValues->insert("key");
#ifdef USE_PCRE
        validParams->insert("use-pcre");
#endif
    }
    else if ("ftp" == thecommand)
    {
        validParams->insert("d");
        validParams->insert("all");
        validParams->insert("tls");
        validParams->insert("public");
        validOptValues->insert("port");
        validOptValues->insert("data-ports");
        validOptValues->insert("certificate");
        validOptValues->insert("key");
#ifdef USE_PCRE
        validParams->insert("use-pcre");
#endif
    }
#endif
    else if ("backup" == thecommand)
    {
        validOptValues->insert("period");
        validOptValues->insert("num-backups");
        validParams->insert("d");
//        validParams->insert("s");
//        validParams->insert("r");
        validParams->insert("a");
//        validParams->insert("i");
        validParams->insert("l");
        validParams->insert("h");
        validOptValues->insert("path-display-size");
        validOptValues->insert("time-format");
    }
    else if ("sync" == thecommand)
    {
        validParams->insert("d");
        validParams->insert("s");
        validParams->insert("r");
        validOptValues->insert("path-display-size");
    }
    else if ("export" == thecommand)
    {
        validParams->insert("a");
        validParams->insert("d");
        validParams->insert("f");
        validOptValues->insert("expire");
        validOptValues->insert("password");
#ifdef USE_PCRE
        validParams->insert("use-pcre");
#endif
    }
    else if ("share" == thecommand)
    {
        validParams->insert("a");
        validParams->insert("d");
        validParams->insert("p");
        validOptValues->insert("with");
        validOptValues->insert("level");
        validOptValues->insert("personal-representation");
#ifdef USE_PCRE
        validParams->insert("use-pcre");
#endif
        validOptValues->insert("time-format");
    }
    else if ("find" == thecommand)
    {
        validOptValues->insert("pattern");
        validOptValues->insert("l");
        validParams->insert("show-handles");
#ifdef USE_PCRE
        validParams->insert("use-pcre");
#endif
        validOptValues->insert("mtime");
        validOptValues->insert("size");
        validOptValues->insert("time-format");
    }
    else if ("mkdir" == thecommand)
    {
        validParams->insert("p");
    }
    else if ("users" == thecommand)
    {
        validParams->insert("s");
        validParams->insert("h");
        validParams->insert("d");
        validParams->insert("n");
        validOptValues->insert("time-format");
    }
    else if ("killsession" == thecommand)
    {
        validParams->insert("a");
    }
    else if ("invite" == thecommand)
    {
        validParams->insert("d");
        validParams->insert("r");
        validOptValues->insert("message");
    }
    else if ("signup" == thecommand)
    {
        validOptValues->insert("name");
    }
    else if ("logout" == thecommand)
    {
        validParams->insert("keep-session");
    }
    else if ("attr" == thecommand)
    {
        validParams->insert("d");
        validParams->insert("s");
    }
    else if ("userattr" == thecommand)
    {
        validOptValues->insert("user");
        validParams->insert("s");
        validParams->insert("list");
    }
    else if ("ipc" == thecommand)
    {
        validParams->insert("a");
        validParams->insert("d");
        validParams->insert("i");
    }
    else if ("showpcr" == thecommand)
    {
        validParams->insert("in");
        validParams->insert("out");
        validOptValues->insert("time-format");
    }
    else if ("thumbnail" == thecommand)
    {
        validParams->insert("s");
    }
    else if ("preview" == thecommand)
    {
        validParams->insert("s");
    }
    else if ("put" == thecommand)
    {
        validParams->insert("c");
        validParams->insert("q");
        validParams->insert("ignore-quota-warn");
        validOptValues->insert("clientID");
    }
    else if ("get" == thecommand)
    {
        validParams->insert("m");
        validParams->insert("q");
        validParams->insert("ignore-quota-warn");
        validOptValues->insert("password");
#ifdef USE_PCRE
        validParams->insert("use-pcre");
#endif
        validOptValues->insert("clientID");
    }
    else if ("import" == thecommand)
    {
        validOptValues->insert("password");
    }
    else if ("login" == thecommand)
    {
        validOptValues->insert("clientID");
        validOptValues->insert("auth-code");
    }
    else if ("psa" == thecommand)
    {
        validParams->insert("discard");
    }
    else if ("reload" == thecommand)
    {
        validOptValues->insert("clientID");
    }
    else if ("transfers" == thecommand)
    {
        validParams->insert("show-completed");
        validParams->insert("summary");
        validParams->insert("only-uploads");
        validParams->insert("only-completed");
        validParams->insert("only-downloads");
        validParams->insert("show-syncs");
        validParams->insert("c");
        validParams->insert("a");
        validParams->insert("p");
        validParams->insert("r");
        validOptValues->insert("limit");
        validOptValues->insert("path-display-size");
    }
    else if ("exit" == thecommand || "quit" == thecommand)
    {
        validParams->insert("only-shell");
    }
#if defined(_WIN32) || defined(__APPLE__)
    else if ("update" == thecommand)
    {
        validOptValues->insert("auto");
    }
#endif
}

void escapeEspace(string &orig)
{
    replaceAll(orig," ", "\\ ");
}

void unescapeEspace(string &orig)
{
    replaceAll(orig,"\\ ", " ");
}

char* empty_completion(const char* text, int state)
{
    // we offer 2 different options so that it doesn't complete (no space is inserted)
    if (state == 0)
    {
        return strdup(" ");
    }
    if (state == 1)
    {
        return strdup(text);
    }
    return NULL;
}

char* generic_completion(const char* text, int state, vector<string> validOptions)
{
    static size_t list_index, len;
    static bool foundone;
    string name;
    if (!validOptions.size()) // no matches
    {
        return empty_completion(text,state); //dont fall back to filenames
    }
    if (!state)
    {
        list_index = 0;
        foundone = false;
        len = strlen(text);
    }
    while (list_index < validOptions.size())
    {
        name = validOptions.at(list_index);
        //Notice: do not escape options for cmdshell. Plus, we won't filter here, because we don't know if the value of rl_completion_quote_chararcter of cmdshell
        // The filtering and escaping will be performed by the completion function in cmdshell
        if (interactiveThread() && !getCurrentThreadIsCmdShell()) {
            escapeEspace(name);
        }

        list_index++;

        if (!( strcmp(text, ""))
                || (( name.size() >= len ) && ( strlen(text) >= len ) &&  ( name.find(text) == 0 ) )
                || getCurrentThreadIsCmdShell()  //do not filter if cmdshell (it will be filter there)
                )
        {
            foundone = true;
            return dupstr((char*)name.c_str());
        }
    }

    if (!foundone)
    {
        return empty_completion(text,state); //dont fall back to filenames
    }

    return((char*)NULL );
}

char* commands_completion(const char* text, int state)
{
    return generic_completion(text, state, validCommands);
}

char* local_completion(const char* text, int state)
{
    return((char*)NULL );
}

void addGlobalFlags(set<string> *setvalidparams)
{
    for (size_t i = 0; i < sizeof( validGlobalParameters ) / sizeof( *validGlobalParameters ); i++)
    {
        setvalidparams->insert(validGlobalParameters[i]);
    }
}

char * flags_completion(const char*text, int state)
{
    static vector<string> validparams;
    if (state == 0)
    {
        validparams.clear();
        char *saved_line = strdup(getCurrentThreadLine().c_str());
        vector<string> words = getlistOfWords(saved_line, !getCurrentThreadIsCmdShell());
        free(saved_line);
        if (words.size())
        {
            set<string> setvalidparams;
            set<string> setvalidOptValues;
            addGlobalFlags(&setvalidparams);

            string thecommand = words[0];
            insertValidParamsPerCommand(&setvalidparams, thecommand, &setvalidOptValues);
            set<string>::iterator it;
            for (it = setvalidparams.begin(); it != setvalidparams.end(); it++)
            {
                string param = *it;
                string toinsert;

                if (param.size() > 1)
                {
                    toinsert = "--" + param;
                }
                else
                {
                    toinsert = "-" + param;
                }

                validparams.push_back(toinsert);
            }

            for (it = setvalidOptValues.begin(); it != setvalidOptValues.end(); it++)
            {
                string param = *it;
                string toinsert;

                if (param.size() > 1)
                {
                    toinsert = "--" + param + '=';
                }
                else
                {
                    toinsert = "-" + param + '=';
                }

                validparams.push_back(toinsert);
            }
        }
    }
    char *toret = generic_completion(text, state, validparams);
    return toret;
}

char * flags_value_completion(const char*text, int state)
{
    static vector<string> validValues;

    if (state == 0)
    {
        validValues.clear();

        char *saved_line = strdup(getCurrentThreadLine().c_str());
        vector<string> words = getlistOfWords(saved_line, !getCurrentThreadIsCmdShell());
        free(saved_line);
        saved_line = NULL;
        if (words.size() > 1)
        {
            string thecommand = words[0];
            string currentFlag = words[words.size() - 1];

            map<string, string> cloptions;
            map<string, int> clflags;

            set<string> validParams;

            insertValidParamsPerCommand(&validParams, thecommand);

            if (setOptionsAndFlags(&cloptions, &clflags, &words, validParams, true))
            {
                // return invalid??
            }

            if (currentFlag.find("--time-format=") == 0)
            {
                string prefix = strncmp(text, "--time-format=", strlen("--time-format="))?"":"--time-format=";
                for (int i = 0; i < MCMDTIME_TOTAL; i++)
                {
                    validValues.push_back(prefix+getTimeFormatNameFromId(i));
                }
            }

            if (thecommand == "share")
            {
                if (currentFlag.find("--level=") == 0)
                {
                    string prefix = strncmp(text, "--level=", strlen("--level="))?"":"--level=";
                    validValues.push_back(prefix+getShareLevelStr(MegaShare::ACCESS_UNKNOWN));
                    validValues.push_back(prefix+getShareLevelStr(MegaShare::ACCESS_READ));
                    validValues.push_back(prefix+getShareLevelStr(MegaShare::ACCESS_READWRITE));
                    validValues.push_back(prefix+getShareLevelStr(MegaShare::ACCESS_FULL));
                    validValues.push_back(prefix+getShareLevelStr(MegaShare::ACCESS_OWNER));
                    validValues.push_back(prefix+getShareLevelStr(MegaShare::ACCESS_UNKNOWN));
                }
                if (currentFlag.find("--with=") == 0)
                {
                    validValues = cmdexecuter->getlistusers();
                    string prefix = strncmp(text, "--with=", strlen("--with="))?"":"--with=";
                    for (unsigned int i=0;i<validValues.size();i++)
                    {
                        validValues.at(i)=prefix+validValues.at(i);
                    }
                }
            }
            else if (( thecommand == "userattr" ) && ( currentFlag.find("--user=") == 0 ))
            {
                validValues = cmdexecuter->getlistusers();
                string prefix = strncmp(text, "--user=", strlen("--user="))?"":"--user=";
                for (unsigned int i=0;i<validValues.size();i++)
                {
                    validValues.at(i)=prefix+validValues.at(i);
                }
            }
            else if  ( ( thecommand == "ftp" || thecommand == "webdav" )
                && ( currentFlag.find("--key=") == 0 || currentFlag.find("--certificate=") == 0 ) )
            {
                const char * cflag = (currentFlag.find("--key=") == 0)? "--key=" : "--certificate=";
                string stext = text;
                size_t begin = strncmp(text, cflag, strlen(cflag))?0:strlen(cflag);
                size_t end = stext.find_last_of('/');
                if (end != string::npos && (end + 1 ) < stext.size() )
                {
                    end = end - begin +1;
                }
                else
                {
                    end = string::npos;
                }

                validValues = cmdexecuter->listlocalpathsstartingby(stext.substr(begin));
                string prefix = strncmp(text, cflag, strlen(cflag))?"":cflag;
                for (unsigned int i=0;i<validValues.size();i++)
                {
                    validValues.at(i)=prefix+validValues.at(i);
                }
            }
        }
    }

    char *toret = generic_completion(text, state, validValues);
    return toret;
}

void unescapeifRequired(string &what)
{
    if (interactiveThread() ) {
        return unescapeEspace(what);
    }
}


char* remotepaths_completion(const char* text, int state, bool onlyfolders)
{
    static vector<string> validpaths;
    if (state == 0)
    {
        string wildtext(text);
        bool usepcre = false; //pcre makes no sense in paths completion
        if (usepcre)
        {
#ifdef USE_PCRE
        wildtext += ".";
#elif __cplusplus >= 201103L
        wildtext += ".";
#endif
        }

        wildtext += "*";

        unescapeEspace(wildtext);

        validpaths = cmdexecuter->listpaths(usepcre, wildtext, onlyfolders);

        // we need to escape '\' to fit what's done when parsing words
        if (!getCurrentThreadIsCmdShell())
        {
            for (int i = 0; i < (int)validpaths.size(); i++)
            {
                replaceAll(validpaths[i],"\\","\\\\");
            }
        }

    }
    return generic_completion(text, state, validpaths);
}

char* remotepaths_completion(const char* text, int state)
{
    return remotepaths_completion(text, state, false);
}

char* remotefolders_completion(const char* text, int state)
{
    return remotepaths_completion(text, state, true);
}

char* loglevels_completion(const char* text, int state)
{
    static vector<string> validloglevels;
    if (state == 0)
    {
        validloglevels.push_back(getLogLevelStr(MegaApi::LOG_LEVEL_FATAL));
        validloglevels.push_back(getLogLevelStr(MegaApi::LOG_LEVEL_ERROR));
        validloglevels.push_back(getLogLevelStr(MegaApi::LOG_LEVEL_WARNING));
        validloglevels.push_back(getLogLevelStr(MegaApi::LOG_LEVEL_INFO));
        validloglevels.push_back(getLogLevelStr(MegaApi::LOG_LEVEL_DEBUG));
        validloglevels.push_back(getLogLevelStr(MegaApi::LOG_LEVEL_MAX));
    }
    return generic_completion(text, state, validloglevels);
}

char* localfolders_completion(const char* text, int state)
{
    static vector<string> validpaths;
    if (state == 0)
    {
        string what(text);
        unescapeEspace(what);
        validpaths = cmdexecuter->listlocalpathsstartingby(what.c_str(), true);
    }
    return generic_completion(text, state, validpaths);
}

char* transfertags_completion(const char* text, int state)
{
    static vector<string> validtransfertags;
    if (state == 0)
    {
        MegaTransferData * transferdata = api->getTransferData();
        if (transferdata)
        {
            for (int i = 0; i < transferdata->getNumUploads(); i++)
            {
                validtransfertags.push_back(SSTR(transferdata->getUploadTag(i)));
            }
            for (int i = 0; i < transferdata->getNumDownloads(); i++)
            {
                validtransfertags.push_back(SSTR(transferdata->getDownloadTag(i)));
            }

            // TODO: reconsider including completed transfers (sth like this:)
//            globalTransferListener->completedTransfersMutex.lock();
//            for (unsigned int i = 0;i < globalTransferListener->completedTransfers.size() && shownCompleted < limit; i++)
//            {
//                MegaTransfer *transfer = globalTransferListener->completedTransfers.at(shownCompleted);
//                if (!transfer->isSyncTransfer())
//                {
//                    validtransfertags.push_back(SSTR(transfer->getTag()));
//                    shownCompleted++;
//                }
//            }
//            globalTransferListener->completedTransfersMutex.unlock();
        }
    }
    return generic_completion(text, state, validtransfertags);
}
char* contacts_completion(const char* text, int state)
{
    static vector<string> validcontacts;
    if (state == 0)
    {
        validcontacts = cmdexecuter->getlistusers();
    }
    return generic_completion(text, state, validcontacts);
}

char* sessions_completion(const char* text, int state)
{
    static vector<string> validSessions;
    if (state == 0)
    {
        validSessions = cmdexecuter->getsessions();
    }

    if (validSessions.size() == 0)
    {
        return empty_completion(text, state);
    }

    return generic_completion(text, state, validSessions);
}

char* nodeattrs_completion(const char* text, int state)
{
    static vector<string> validAttrs;
    if (state == 0)
    {
        validAttrs.clear();
        char *saved_line = strdup(getCurrentThreadLine().c_str());
        vector<string> words = getlistOfWords(saved_line, !getCurrentThreadIsCmdShell());
        free(saved_line);
        saved_line = NULL;
        if (words.size() > 1)
        {
            validAttrs = cmdexecuter->getNodeAttrs(words[1]);
        }
    }

    if (validAttrs.size() == 0)
    {
        return empty_completion(text, state);
    }

    return generic_completion(text, state, validAttrs);
}

char* userattrs_completion(const char* text, int state)
{
    static vector<string> validAttrs;
    if (state == 0)
    {
        validAttrs.clear();
        validAttrs = cmdexecuter->getUserAttrs();
    }

    if (validAttrs.size() == 0)
    {
        return empty_completion(text, state);
    }

    return generic_completion(text, state, validAttrs);
}

completionfunction_t *getCompletionFunction(vector<string> words)
{
    // Strip words without flags
    string thecommand = words[0];

    if (words.size() > 1)
    {
        string lastword = words[words.size() - 1];
        if (lastword.find_first_of("-") == 0)
        {
            if (lastword.find_last_of("=") != string::npos)
            {
                return flags_value_completion;
            }
            else
            {
                return flags_completion;
            }
        }
    }
    discardOptionsAndFlags(&words);

    int currentparameter = int(words.size() - 1);
    if (stringcontained(thecommand.c_str(), localremotefolderpatterncommands))
    {
        if (currentparameter == 1)
        {
            return local_completion;
        }
        if (currentparameter == 2)
        {
            return remotefolders_completion;
        }
    }
    else if (thecommand == "put")
    {
        if (currentparameter == 1)
        {
            return local_completion;
        }
        else
        {
            return remotepaths_completion;
        }
    }
    else if (thecommand == "backup")
    {
        if (currentparameter == 1)
        {
            return localfolders_completion;
        }
        else
        {
            return remotefolders_completion;
        }
    }
    else if (stringcontained(thecommand.c_str(), remotepatterncommands))
    {
        if (currentparameter == 1)
        {
            return remotepaths_completion;
        }
    }
    else if (stringcontained(thecommand.c_str(), remotefolderspatterncommands))
    {
        if (currentparameter == 1)
        {
            return remotefolders_completion;
        }
    }
    else if (stringcontained(thecommand.c_str(), multipleremotepatterncommands))
    {
        if (currentparameter >= 1)
        {
            return remotepaths_completion;
        }
    }
    else if (stringcontained(thecommand.c_str(), localfolderpatterncommands))
    {
        if (currentparameter == 1)
        {
            return localfolders_completion;
        }
    }
    else if (stringcontained(thecommand.c_str(), remoteremotepatterncommands))
    {
        if (( currentparameter == 1 ) || ( currentparameter == 2 ))
        {
            return remotepaths_completion;
        }
    }
    else if (stringcontained(thecommand.c_str(), remotelocalpatterncommands))
    {
        if (currentparameter == 1)
        {
            return remotepaths_completion;
        }
        if (currentparameter == 2)
        {
            return local_completion;
        }
    }
    else if (stringcontained(thecommand.c_str(), emailpatterncommands))
    {
        if (currentparameter == 1)
        {
            return contacts_completion;
        }
    }
    else if (thecommand == "import")
    {
        if (currentparameter == 2)
        {
            return remotepaths_completion;
        }
    }
    else if (thecommand == "killsession")
    {
        if (currentparameter == 1)
        {
            return sessions_completion;
        }
    }
    else if (thecommand == "attr")
    {
        if (currentparameter == 1)
        {
            return remotepaths_completion;
        }
        if (currentparameter == 2)
        {
            return nodeattrs_completion;
        }
    }
    else if (thecommand == "userattr")
    {
        if (currentparameter == 1)
        {
            return userattrs_completion;
        }
    }
    else if (thecommand == "log")
    {
        if (currentparameter == 1)
        {
            return loglevels_completion;
        }
    }
    else if (thecommand == "transfers")
    {
        if (currentparameter == 1)
        {
            return transfertags_completion;
        }
    }
    return empty_completion;
}

string getListOfCompletionValues(vector<string> words, char separator = ' ', const char * separators = " :;!`\"'\\()[]{}<>", bool suppressflag = true)
{
    string completionValues;
    completionfunction_t * compfunction = getCompletionFunction(words);
    if (compfunction == local_completion)
    {
        if (!interactiveThread())
        {
            return "MEGACMD_USE_LOCAL_COMPLETION";
        }
        else
        {
            string toret="MEGACMD_USE_LOCAL_COMPLETION";
            toret+=cmdexecuter->getLPWD();
            return toret;
        }
    }
#ifdef _WIN32
//    // let MEGAcmdShell handle the local folder completion (available via autocomplete.cpp stuff that takes into account units/unicode/etc...)
//    else if (compfunction == localfolders_completion)
//    {
//        if (!interactiveThread())
//        {
//            return "MEGACMD_USE_LOCAL_COMPLETIONFOLDERS";
//        }
//        else
//        {
//            string toret="MEGACMD_USE_LOCAL_COMPLETIONFOLDERS";
//            toret+=cmdexecuter->getLPWD();
//            return toret;
//        }
//    }
#endif
    int state=0;
    if (words.size()>1)
    while (true)
    {
        char *newval;
        string &lastword = words[words.size()-1];
        if (suppressflag && lastword.size()>3 && lastword[0]== '-' && lastword[1]== '-' && lastword.find('=')!=string::npos)
        {
            newval = compfunction(lastword.substr(lastword.find_first_of('=')+1).c_str(), state);
        }
        else
        {
            newval = compfunction(lastword.c_str(), state);
        }

        if (!newval) break;
        if (completionValues.size())
        {
            completionValues+=separator;
        }

        string snewval=newval;
        if (snewval.find_first_of(separators) != string::npos)
        {
            completionValues+="\"";
            replaceAll(snewval,"\"","\\\"");
            completionValues+=snewval;
            completionValues+="\"";
        }
        else
        {
            completionValues+=newval;
        }
        free(newval);

        state++;
    }
    return completionValues;
}

MegaApi* getFreeApiFolder()
{
    semaphoreapiFolders.wait();
    mutexapiFolders.lock();
    MegaApi* toret = apiFolders.front();
    apiFolders.pop();
    occupiedapiFolders.push_back(toret);
    mutexapiFolders.unlock();
    return toret;
}

void freeApiFolder(MegaApi *apiFolder)
{
    mutexapiFolders.lock();
    occupiedapiFolders.erase(std::remove(occupiedapiFolders.begin(), occupiedapiFolders.end(), apiFolder), occupiedapiFolders.end());
    apiFolders.push(apiFolder);
    semaphoreapiFolders.release();
    mutexapiFolders.unlock();
}

const char * getUsageStr(const char *command)
{
    if (!strcmp(command, "login"))
    {
        if (interactiveThread())
        {
            return "login [--auth-code=XXXX] [email [password]] | exportedfolderurl#key | session";
        }
        else
        {
            return "login [--auth-code=XXXX] email password | exportedfolderurl#key | session";
        }
    }
    if (!strcmp(command, "psa"))
    {
        return "psa [--discard]";
    }
    if (!strcmp(command, "cancel"))
    {
        return "cancel";
    }
    if (!strcmp(command, "confirmcancel"))
    {
        if (interactiveThread())
        {
            return "confirmcancel link [password]";
        }
        else
        {
            return "confirmcancel link password";
        }
    }
    if (!strcmp(command, "begin"))
    {
        return "begin [ephemeralhandle#ephemeralpw]";
    }
    if (!strcmp(command, "signup"))
    {
        if (interactiveThread())
        {
            return "signup email [password] [--name=\"Your Name\"]";
        }
        else
        {
            return "signup email password [--name=\"Your Name\"]";
        }
    }
    if (!strcmp(command, "confirm"))
    {
        if (interactiveThread())
        {
            return "confirm link email [password]";
        }
        else
        {
            return "confirm link email password";
        }
    }
    if (!strcmp(command, "errorcode"))
    {
        return "errorcode number";
    }
    if (!strcmp(command, "graphics"))
    {
        return "graphics [on|off]";
    }
    if (!strcmp(command, "session"))
    {
        return "session";
    }
    if (!strcmp(command, "mount"))
    {
        return "mount";
    }
#if defined(_WIN32) && !defined(NO_READLINE)
    if (!strcmp(command, "unicode"))
    {
        return "unicode";
    }
#endif
    if (!strcmp(command, "ls"))
    {
#ifdef USE_PCRE
        return "ls [-halRr] [--show-handles] [--tree] [--versions] [remotepath] [--use-pcre] [--time-format=FORMAT]";
#else
        return "ls [-halRr] [--show-handles] [--tree] [--versions] [remotepath] [--time-format=FORMAT]";
#endif
    }
    if (!strcmp(command, "tree"))
    {
        return "tree [remotepath]";
    }
    if (!strcmp(command, "cd"))
    {
        return "cd [remotepath]";
    }
    if (!strcmp(command, "log"))
    {
        return "log [-sc] level";
    }
    if (!strcmp(command, "du"))
    {
#ifdef USE_PCRE
        return "du [-h] [--versions] [remotepath remotepath2 remotepath3 ... ] [--use-pcre]";
#else
        return "du [-h] [--versions] [remotepath remotepath2 remotepath3 ... ]";
#endif
    }
    if (!strcmp(command, "pwd"))
    {
        return "pwd";
    }
    if (!strcmp(command, "lcd"))
    {
        return "lcd [localpath]";
    }
    if (!strcmp(command, "lpwd"))
    {
        return "lpwd";
    }
    if (!strcmp(command, "import"))
    {
        return "import exportedlink [--password=PASSWORD] [remotepath]";
    }
    if (!strcmp(command, "put"))
    {
        return "put  [-c] [-q] [--ignore-quota-warn] localfile [localfile2 localfile3 ...] [dstremotepath]";
    }
    if (!strcmp(command, "putq"))
    {
        return "putq [cancelslot]";
    }
    if (!strcmp(command, "get"))
    {
#ifdef USE_PCRE
        return "get [-m] [-q] [--ignore-quota-warn] [--use-pcre] [--password=PASSWORD] exportedlink|remotepath [localpath]";
#else
        return "get [-m] [-q] [--ignore-quota-warn] [--password=PASSWORD] exportedlink|remotepath [localpath]";
#endif
    }
    if (!strcmp(command, "getq"))
    {
        return "getq [cancelslot]";
    }
    if (!strcmp(command, "pause"))
    {
        return "pause [get|put] [hard] [status]";
    }
    if (!strcmp(command, "attr"))
    {
        return "attr remotepath [-s attribute value|-d attribute]";
    }
    if (!strcmp(command, "userattr"))
    {
        return "userattr [-s attribute value|attribute|--list] [--user=user@email]";
    }
    if (!strcmp(command, "mkdir"))
    {
        return "mkdir [-p] remotepath";
    }
    if (!strcmp(command, "rm"))
    {
#ifdef USE_PCRE
        return "rm [-r] [-f] [--use-pcre] remotepath";
#else
        return "rm [-r] [-f] remotepath";
#endif
    }
    if (!strcmp(command, "mv"))
    {
#ifdef USE_PCRE
        return "mv srcremotepath [--use-pcre] [srcremotepath2 srcremotepath3 ..] dstremotepath";
#else
        return "mv srcremotepath [srcremotepath2 srcremotepath3 ..] dstremotepath";
#endif
    }
    if (!strcmp(command, "cp"))
    {
#ifdef USE_PCRE
        return "cp [--use-pcre] srcremotepath [srcremotepath2 srcremotepath3 ..] dstremotepath|dstemail:";
#else
        return "cp srcremotepath [srcremotepath2 srcremotepath3 ..] dstremotepath|dstemail:";
#endif
    }
    if (!strcmp(command, "deleteversions"))
    {
#ifdef USE_PCRE
        return "deleteversions [-f] (--all | remotepath1 remotepath2 ...)  [--use-pcre]";
#else
        return "deleteversions [-f] (--all | remotepath1 remotepath2 ...)";
#endif

    }
    if (!strcmp(command, "exclude"))
    {
        return "exclude [(-a|-d) pattern1 pattern2 pattern3 [--restart-syncs]]";
    }
#ifdef HAVE_LIBUV
    if (!strcmp(command, "webdav"))
    {
#ifdef USE_PCRE
        return "webdav [-d (--all | remotepath ) ] [ remotepath [--port=PORT] [--public] [--tls --certificate=/path/to/certificate.pem --key=/path/to/certificate.key]] [--use-pcre]";
#else
        return "webdav [-d (--all | remotepath ) ] [ remotepath [--port=PORT] [--public] [--tls --certificate=/path/to/certificate.pem --key=/path/to/certificate.key]]";
#endif
    }
    if (!strcmp(command, "ftp"))
    {
#ifdef USE_PCRE
        return "ftp [-d ( --all | remotepath ) ] [ remotepath [--port=PORT] [--data-ports=BEGIN-END] [--public] [--tls --certificate=/path/to/certificate.pem --key=/path/to/certificate.key]] [--use-pcre]";
#else
        return "ftp [-d ( --all | remotepath ) ] [ remotepath [--port=PORT] [--data-ports=BEGIN-END] [--public] [--tls --certificate=/path/to/certificate.pem --key=/path/to/certificate.key]]";
#endif
    }
#endif
    if (!strcmp(command, "sync"))
    {
        return "sync [localpath dstremotepath| [-dsr] [ID|localpath]";
    }
    if (!strcmp(command, "backup"))
    {
        return "backup (localpath remotepath --period=\"PERIODSTRING\" --num-backups=N  | [-lhda] [TAG|localpath] [--period=\"PERIODSTRING\"] [--num-backups=N]) [--time-format=FORMAT]";
    }
    if (!strcmp(command, "https"))
    {
        return "https [on|off]";
    }
#ifndef _WIN32
    if (!strcmp(command, "permissions"))
    {
        return "permissions [(--files|--folders) [-s XXX]]";
    }
#endif
    if (!strcmp(command, "export"))
    {
#ifdef USE_PCRE
        return "export [-d|-a [--password=PASSWORD] [--expire=TIMEDELAY] [-f]] [remotepath] [--use-pcre] [--time-format=FORMAT]";
#else
        return "export [-d|-a [--password=PASSWORD] [--expire=TIMEDELAY] [-f]] [remotepath] [--time-format=FORMAT]";
#endif
    }
    if (!strcmp(command, "share"))
    {
#ifdef USE_PCRE
        return "share [-p] [-d|-a --with=user@email.com [--level=LEVEL]] [remotepath] [--use-pcre] [--time-format=FORMAT]";
#else
        return "share [-p] [-d|-a --with=user@email.com [--level=LEVEL]] [remotepath] [--time-format=FORMAT]";
#endif
    }
    if (!strcmp(command, "invite"))
    {
        return "invite [-d|-r] dstemail [--message=\"MESSAGE\"]";
    }
    if (!strcmp(command, "ipc"))
    {
        return "ipc email|handle -a|-d|-i";
    }
    if (!strcmp(command, "showpcr"))
    {
        return "showpcr [--in | --out] [--time-format=FORMAT]";
    }
    if (!strcmp(command, "masterkey"))
    {
        return "masterkey pathtosave";
    }
    if (!strcmp(command, "users"))
    {
        return "users [-s] [-h] [-n] [-d contact@email] [--time-format=FORMAT]";
    }
    if (!strcmp(command, "getua"))
    {
        return "getua attrname [email]";
    }
    if (!strcmp(command, "putua"))
    {
        return "putua attrname [del|set string|load file]";
    }
    if (!strcmp(command, "speedlimit"))
    {
        return "speedlimit [-u|-d] [-h] [NEWLIMIT]";
    }
    if (!strcmp(command, "killsession"))
    {
        return "killsession [-a | sessionid1 sessionid2 ... ]";
    }
    if (!strcmp(command, "whoami"))
    {
        return "whoami [-l]";
    }
    if (!strcmp(command, "df"))
    {
        return "df [-h]";
    }
    if (!strcmp(command, "cat"))
    {
        return "cat remotepath1 remotepath2 ...";
    }
    if (!strcmp(command, "mediainfo"))
    {
        return "info remotepath1 remotepath2 ...";
    }
    if (!strcmp(command, "passwd"))
    {
        if (interactiveThread())
        {
            return "passwd [-f]  [--auth-code=XXXX] [newpassword]";
        }
        else
        {
            return "passwd [-f]  [--auth-code=XXXX] newpassword";
        }
    }
    if (!strcmp(command, "retry"))
    {
        return "retry";
    }
    if (!strcmp(command, "recon"))
    {
        return "recon";
    }
    if (!strcmp(command, "reload"))
    {
        return "reload";
    }
    if (!strcmp(command, "logout"))
    {
        return "logout [--keep-session]";
    }
    if (!strcmp(command, "symlink"))
    {
        return "symlink";
    }
    if (!strcmp(command, "version"))
    {
        return "version [-l][-c]";
    }
    if (!strcmp(command, "debug"))
    {
        return "debug";
    }
    if (!strcmp(command, "chatf"))
    {
        return "chatf ";
    }
    if (!strcmp(command, "chatc"))
    {
        return "chatc group [email ro|rw|full|op]*";
    }
    if (!strcmp(command, "chati"))
    {
        return "chati chatid email ro|rw|full|op";
    }
    if (!strcmp(command, "chatr"))
    {
        return "chatr chatid [email]";
    }
    if (!strcmp(command, "chatu"))
    {
        return "chatu chatid";
    }
    if (!strcmp(command, "chatga"))
    {
        return "chatga chatid nodehandle uid";
    }
    if (!strcmp(command, "chatra"))
    {
        return "chatra chatid nodehandle uid";
    }
    if (!strcmp(command, "exit"))
    {
        return "exit [--only-shell]";
    }
    if (!strcmp(command, "quit"))
    {
        return "quit [--only-shell]";
    }
    if (!strcmp(command, "history"))
    {
        return "history";
    }
    if (!strcmp(command, "thumbnail"))
    {
        return "thumbnail [-s] remotepath localpath";
    }
    if (!strcmp(command, "preview"))
    {
        return "preview [-s] remotepath localpath";
    }
    if (!strcmp(command, "find"))
    {
#ifdef USE_PCRE
        return "find [remotepath] [-l] [--pattern=PATTERN] [--mtime=TIMECONSTRAIN] [--size=SIZECONSTRAIN] [--use-pcre] [--time-format=FORMAT] [--show-handles]";
#else
        return "find [remotepath] [-l] [--pattern=PATTERN] [--mtime=TIMECONSTRAIN] [--size=SIZECONSTRAIN] [--time-format=FORMAT] [--show-handles]";
#endif
    }
    if (!strcmp(command, "help"))
    {
        return "help [-f]";
    }
    if (!strcmp(command, "clear"))
    {
        return "clear";
    }
    if (!strcmp(command, "transfers"))
    {
        return "transfers [-c TAG|-a] | [-r TAG|-a]  | [-p TAG|-a] [--only-downloads | --only-uploads] [SHOWOPTIONS]";
    }
#if defined(_WIN32) && defined(NO_READLINE)
    if (!strcmp(command, "autocomplete"))
    {
        return "autocomplete [dos | unix]";
    }
    else if (!strcmp(command, "codepage"))
    {
        return "codepage [N [M]]";
    }
#endif
#if defined(_WIN32) || defined(__APPLE__)
    if (!strcmp(command, "update"))
    {
        return "update [--auto=on|off|query]";
    }
#endif
    return "command not found: ";
}

bool validCommand(string thecommand)
{
    return stringcontained((char*)thecommand.c_str(), validCommands);
}

string getsupportedregexps()
{
#ifdef USE_PCRE
        return "Perl Compatible Regular Expressions with \"--use-pcre\"\n   or wildcarded expresions with ? or * like f*00?.txt";
#elif __cplusplus >= 201103L
        return "c++11 Regular Expressions";
#else
        return "it accepts wildcards: ? and *. e.g.: f*00?.txt";
#endif
}

void printTimeFormatHelp(ostringstream &os)
{
    os << " --time-format=FORMAT" << "\t" << "show time in available formats. Examples:" << endl;
    os << "               RFC2822: " << " Example: Fri, 06 Apr 2018 13:05:37 +0200" << endl;
    os << "               ISO6081: " << " Example: 2018-04-06" << endl;
    os << "               ISO6081_WITH_TIME: " << " Example: 2018-04-06T13:05:37" << endl;
    os << "               SHORT: " << " Example: 06Apr2018 13:05:37" << endl;
    os << "               SHORT_UTC: " << " Example: 06Apr2018 13:05:37" << endl;
    os << "               CUSTOM. e.g: --time-format=\"%Y %b\": "<< " Example: 2018 Apr" << endl;
}

string getHelpStr(const char *command)
{
    ostringstream os;

    os << "Usage: " << getUsageStr(command) << endl;
    if (!strcmp(command, "login"))
    {
        os << "Logs into a MEGA account" << endl;
        os << " You can log in either with email and password, with session ID," << endl;
        os << " or into a folder (an exported/public folder)" << endl;
        os << " If logging into a folder indicate url#key" << endl;
        os << endl;
        os << "Options:" << endl;
        os << " --auth-code=XXXX" << "\t" << "Two-factor Authentication code. More info: https://mega.nz/blog_48" << endl;
    }
    else if (!strcmp(command, "cancel"))
    {
        os << "Cancels your MEGA account" << endl;
        os << " Caution: The account under this email address will be permanently closed" << endl;
        os << " and your data deleted. This can not be undone." << endl;
        os << endl;
        os << "The cancellation will not take place immediately. You will need to confirm the cancellation" << endl;
        os << "using a link that will be delivered to your email. See \"confirmcancel --help\"" << endl;
    }
    else if (!strcmp(command, "psa"))
    {
        os << "Shows the next available Public Service Announcement (PSA)" << endl;
        os << endl;
        os << "Options:" << endl;
        os << " --discard" << "\t" << "Discards last received PSA" << endl;
        os << endl;
    }
    else if (!strcmp(command, "confirmcancel"))
    {
        os << "Confirms the cancellation of your MEGA account" << endl;
        os << " Caution: The account under this email address will be permanently closed" << endl;
        os << " and your data deleted. This can not be undone." << endl;
    }
    else if (!strcmp(command, "errorcode"))
    {
        os << "Translate error code into string" << endl;
    }
    else if (!strcmp(command, "graphics"))
    {
        os << "Shows if special features related to images and videos are enabled. " << endl;
        os << "Use \"graphics on/off\" to enable/disable it." << endl;
        os << endl;
        os << "Disabling these features will avoid the upload of previews and thumbnails" << endl;
        os << "for images and videos." << endl;
        os << endl;
        os << "It's only recommended to disable these features before uploading files" << endl;
        os << "with image or video extensions that are not really images or videos," << endl;
        os << "or that are encrypted in the local drive so they can't be analyzed anyway." << endl;
        os << endl;
        os << "Notice that this setting will be saved for the next time you open MEGAcmd" << endl;
    }
    else if (!strcmp(command, "signup"))
    {
        os << "Register as user with a given email" << endl;
        os << endl;
        os << "Options:" << endl;
        os << " --name=\"Your Name\"" << "\t" << "Name to register. e.g. \"John Smith\"" << endl;
        os << endl;
        os << " You will receive an email to confirm your account. " << endl;
        os << " Once you have received the email, please proceed to confirm the link " << endl;
        os << " included in that email with \"confirm\"." << endl;
        os << endl;
        os << "Warning: Due to our end-to-end encryption paradigm, you will not be able to access your data " << endl;
        os << "without either your password or a backup of your Recovery Key (master key)." << endl;
        os << "Exporting the master key and keeping it in a secure location enables you " << endl;
        os << "to set a new password without data loss. Always keep physical control of " << endl;
        os << "your master key (e.g. on a client device, external storage, or print)." << endl;
        os << " See \"masterkey --help\" for further info." << endl;
    }
    else if (!strcmp(command, "clear"))
    {
        os << "Clear screen" << endl;
    }
    else if (!strcmp(command, "help"))
    {
        os << "Prints list of commands" << endl;
        os << endl;
        os << "Options:" << endl;
        os << " -f" << "\t" << "Include a brief description of the commands" << endl;
    }
    else if (!strcmp(command, "history"))
    {
        os << "Prints history of used commands" << endl;
        os << "  Only commands used in interactive mode are registered" << endl;
    }
    else if (!strcmp(command, "confirm"))
    {
        os << "Confirm an account using the link provided after the \"signup\" process." << endl;
        os << " It requires the email and the password used to obtain the link." << endl;
        os << endl;
    }
    else if (!strcmp(command, "session"))
    {
        os << "Prints (secret) session ID" << endl;
    }
    else if (!strcmp(command, "mount"))
    {
        os << "Lists all the main nodes" << endl;
    }
#if defined(_WIN32) && !defined(NO_READLINE)
    else if (!strcmp(command, "unicode"))
    {
        os << "Toggle unicode input enabled/disabled in interactive shell" << endl;
        os << endl;
        os << " Unicode mode is experimental, you might experience" << endl;
        os << " some issues interacting with the console" << endl;
        os << " (e.g. history navigation fails)." << endl;
        os << "Type \"help --unicode\" for further info" << endl;
    }
#endif
    else if (!strcmp(command, "ls"))
    {
        os << "Lists files in a remote path" << endl;
        os << " remotepath can be a pattern (" << getsupportedregexps() << ") " << endl;
        os << " Also, constructions like /PATTERN1/PATTERN2/PATTERN3 are allowed" << endl;
        os << endl;
        os << "Options:" << endl;
        os << " -R|-r" << "\t" << "List folders recursively" << endl;
        os << " --tree" << "\t" << "Prints tree-like exit (implies -r)" << endl;
        os << " --show-handles" << "\t" << "Prints files/folders handles (H:XXXXXXXX). You can address a file/folder by its handle" << endl;
        os << " -l" << "\t" << "Print summary (--tree has no effect)" << endl;
        os << "   " << "\t" << " SUMMARY contents:" << endl;
        os << "   " << "\t" << "   FLAGS: Indicate type/status of an element:" << endl;
        os << "   " << "\t" << "     xxxx" << endl;
        os << "   " << "\t" << "     |||+---- Sharing status: (s)hared, (i)n share or not shared(-)" << endl;
        os << "   " << "\t" << "     ||+----- if exported, whether it is (p)ermanent or (t)temporal" << endl;
        os << "   " << "\t" << "     |+------ e/- wheter node is (e)xported" << endl;
        os << "   " << "\t" << "     +-------- Type(d=folder,-=file,r=root,i=inbox,b=rubbish,x=unsupported)" << endl;
        os << "   " << "\t" << "   VERS: Number of versions in a file" << endl;
        os << "   " << "\t" << "   SIZE: Size of the file in bytes:" << endl;
        os << "   " << "\t" << "   DATE: Modification date for files and creation date for folders (in UTC time):" << endl;
        os << "   " << "\t" << "   NAME: name of the node" << endl;
        os << " -h" << "\t" << "Show human readable sizes in summary" << endl;
        os << " -a" << "\t" << "Include extra information" << endl;
        os << "   " << "\t" << " If this flag is repeated (e.g: -aa) more info will appear" << endl;
        os << "   " << "\t" << " (public links, expiration dates, ...)" << endl;
        os << " --versions" << "\t" << "show historical versions" << endl;
        os << "   " << "\t" << "You can delete all versions of a file with \"deleteversions\"" << endl;
        printTimeFormatHelp(os);

#ifdef USE_PCRE
        os << " --use-pcre" << "\t" << "use PCRE expressions" << endl;
#endif
    }
    else if (!strcmp(command, "tree"))
    {
        os << "Lists files in a remote path in a nested tree decorated output" << endl;
        os << endl;
        os << "This is similar to \"ls --tree\"" << endl;
    }
#if defined(_WIN32) || defined(__APPLE__)
    else if (!strcmp(command, "update"))
    {
        os << "Updates MEGAcmd" << endl;
        os << endl;
        os << "Looks for updates and applies if available." << endl;
        os << "This command can also be used to enable/disable automatic updates." << endl;

        os << "Options:" << endl;
        os << " --auto=ON|OFF|query" << "\t" << "Enables/disables/queries status of auto updates." << endl;
        os << endl;
        os << "If auto updates are enabled it will be checked while MEGAcmd server is running." << endl;
        os << " If there is an update available, it will be downloaded and applied. " << endl;
        os << " This will cause MEGAcmd to be restarted whenever the updates are applied." << endl;
        os << endl;
        os << "Further info at https://github.com/meganz/megacmd#megacmd-updates";
    }
#endif
    else if (!strcmp(command, "cd"))
    {
        os << "Changes the current remote folder" << endl;
        os << endl;
        os << "If no folder is provided, it will be changed to the root folder" << endl;
    }
    else if (!strcmp(command, "log"))
    {
        os << "Prints/Modifies the current logs level" << endl;
        os << endl;
        os << "Options:" << endl;
        os << " -c" << "\t" << "CMD log level (higher level messages). " << endl;
        os << "   " << "\t" << " Messages captured by MEGAcmd server." << endl;
        os << " -s" << "\t" << "SDK log level (lower level messages)." << endl;
        os << "   " << "\t" << " Messages captured by the engine and libs" << endl;

        os << endl;
        os << "Regardless of the log level of the" << endl;
        os << " interactive shell, you can increase the amount of information given" <<  endl;
        os << "   by any command by passing \"-v\" (\"-vv\", \"-vvv\", ...)" << endl;


    }
    else if (!strcmp(command, "du"))
    {
        os << "Prints size used by files/folders" << endl;
        os << " remotepath can be a pattern (" << getsupportedregexps() << ") " << endl;
        os << endl;
        os << "Options:" << endl;
        os << " -h" << "\t" << "Human readable" << endl;
        os << " --versions" << "\t" << "Calculate size including all versions." << endl;
        os << "   " << "\t" << "You can remove all versions with \"deleteversions\" and list them with \"ls --versions\"" << endl;
        os << " --path-display-size=N" << "\t" << "Use a fixed size of N characters for paths" << endl;

#ifdef USE_PCRE
        os << " --use-pcre" << "\t" << "use PCRE expressions" << endl;
#endif
    }
    else if (!strcmp(command, "pwd"))
    {
        os << "Prints the current remote folder" << endl;
    }
    else if (!strcmp(command, "lcd"))
    {
        os << "Changes the current local folder for the interactive console" << endl;
        os << endl;
        os << "It will be used for uploads and downloads" << endl;
        os << endl;
        os << "If not using interactive console, the current local folder will be " << endl;
        os << " that of the shell executing mega comands" << endl;
    }
    else if (!strcmp(command, "lpwd"))
    {
        os << "Prints the current local folder for the interactive console" << endl;
        os << endl;
        os << "It will be used for uploads and downloads" << endl;
        os << endl;
        os << "If not using interactive console, the current local folder will be " << endl;
        os << " that of the shell executing mega comands" << endl;
    }
    else if (!strcmp(command, "logout"))
    {
        os << "Logs out" << endl;
        os << endl;
        os << "Options:" << endl;
        os << " --keep-session" << "\t" << "Keeps the current session." << endl;
    }
    else if (!strcmp(command, "import"))
    {
        os << "Imports the contents of a remote link into user's cloud" << endl;
        os << endl;
        os << "If no remote path is provided, the current local folder will be used" << endl;
        os << "Exported links: Exported links are usually formed as publiclink#key." << endl;
        os << " Alternativelly you can provide a password-protected link and" << endl;
        os << " provide the password with --password" << endl;
    }
    else if (!strcmp(command, "put"))
    {
        os << "Uploads files/folders to a remote folder" << endl;
        os << endl;
        os << "Options:" << endl;
        os << " -c" << "\t" << "Creates remote folder destination in case of not existing." << endl;
        os << " -q" << "\t" << "queue upload: execute in the background. Don't wait for it to end' " << endl;
        os << " --ignore-quota-warn" << "\t" << "ignore quota surpassing warning. " << endl;
        os << "                    " << "\t" << "  The upload will be attempted anyway." << endl;

        os << endl;
        os << "Notice that the dstremotepath can only be omitted when only one local path is provided. " << endl;
        os << " In such case, the current remote working dir will be the destination for the upload." << endl;
        os << " Mind that using wildcards for local paths in non-interactive mode in a supportive console (e.g. bash)," << endl;
        os << " could result in multiple paths being passed to MEGAcmd." << endl;
    }
    else if (!strcmp(command, "get"))
    {
        os << "Downloads a remote file/folder or a public link " << endl;
        os << endl;
        os << "In case it is a file, the file will be downloaded at the specified folder " << endl;
        os << "                             (or at the current folder if none specified)." << endl;
        os << "  If the localpath (destination) already exists and is the same (same contents)" << endl;
        os << "  nothing will be done. If differs, it will create a new file appending \" (NUM)\" " << endl;
        os << endl;
        os << "For folders, the entire contents (and the root folder itself) will be" << endl;
        os << "                    by default downloaded into the destination folder" << endl;
        os << endl;
        os << "Exported links: Exported links are usually formed as publiclink#key." << endl;
        os << " Alternativelly you can provide a password-protected link and" << endl;
        os << " provide the password with --password" << endl;
        os << endl;
        os << "Options:" << endl;
        os << " -q" << "\t" << "queue download: execute in the background. Don't wait for it to end' " << endl;
        os << " -m" << "\t" << "if the folder already exists, the contents will be merged with the " << endl;
        os << "                     downloaded one (preserving the existing files)" << endl;
        os << " --ignore-quota-warn" << "\t" << "ignore quota surpassing warning. " << endl;
        os << "                    " << "\t" << "  The download will be attempted anyway." << endl;
        os << " --password=PASSWORD" << "\t" << "Password to decrypt the password-protected link." << endl;
#ifdef USE_PCRE
        os << " --use-pcre" << "\t" << "use PCRE expressions" << endl;
#endif

    }
    if (!strcmp(command, "attr"))
    {
        os << "Lists/updates node attributes" << endl;
        os << endl;
        os << "Options:" << endl;
        os << " -s" << "\tattribute value \t" << "sets an attribute to a value" << endl;
        os << " -d" << "\tattribute       \t" << "removes the attribute" << endl;
    }
    if (!strcmp(command, "userattr"))
    {
        os << "Lists/updates user attributes" << endl;
        os << endl;
        os << "Options:" << endl;
        os << " -s" << "\tattribute value \t" << "sets an attribute to a value" << endl;
        os << " --user=user@email" << "\t" << "select the user to query" << endl;
        os << " --list" << "\t" << "lists valid attributes" << endl;
    }
    else if (!strcmp(command, "mkdir"))
    {
        os << "Creates a directory or a directories hierarchy" << endl;
        os << endl;
        os << "Options:" << endl;
        os << " -p" << "\t" << "Allow recursive" << endl;
    }
    else if (!strcmp(command, "rm"))
    {
        os << "Deletes a remote file/folder" << endl;
        os << endl;
        os << "Options:" << endl;
        os << " -r" << "\t" << "Delete recursively (for folders)" << endl;
        os << " -f" << "\t" << "Force (no asking)" << endl;
#ifdef USE_PCRE
        os << " --use-pcre" << "\t" << "use PCRE expressions" << endl;
#endif
    }
    else if (!strcmp(command, "mv"))
    {
        os << "Moves file(s)/folder(s) into a new location (all remotes)" << endl;
        os << endl;
        os << "If the location exists and is a folder, the source will be moved there" << endl;
        os << "If the location doesn't exist, the source will be renamed to the destination name given" << endl;
#ifdef USE_PCRE
        os << "Options:" << endl;
        os << " --use-pcre" << "\t" << "use PCRE expressions" << endl;
#endif
    }
    else if (!strcmp(command, "cp"))
    {
        os << "Copies files/folders into a new location (all remotes)" << endl;
        os << endl;
        os << "If the location exists and is a folder, the source will be copied there" << endl;
        os << "If the location doesn't exist, and only one source is provided," << endl;
        os << " the file/folder will be copied and renamed to the destination name given." << endl;
        os << endl;
        os << "If \"dstemail:\" provided, the file/folder will be sent to that user's inbox (//in)" << endl;
        os << " e.g: cp /path/to/file user@doma.in:" << endl;
        os << " Remember the trailing \":\", otherwise a file with the name of that user (\"user@doma.in\") will be created" << endl;
#ifdef USE_PCRE
        os << "Options:" << endl;
        os << " --use-pcre" << "\t" << "use PCRE expressions" << endl;
#endif
    }
#ifndef _WIN32
    else if (!strcmp(command, "permissions"))
    {
        os << "Shows/Establish default permissions for files and folders created by MEGAcmd." << endl;
        os << endl;
        os << "Permissions are unix-like permissions, with 3 numbers: one for owner, one for group and one for others" << endl;
        os << "Options:" << endl;
        os << " --files" << "\t" << "To show/set files default permissions." << endl;
        os << " --folders" << "\t" << "To show/set folders default permissions." << endl;
        os << " --s XXX" << "\t" << "To set new permissions for newly created files/folder. " << endl;
        os << "        " << "\t" << " Notice that for files minimum permissions is 600," << endl;
        os << "        " << "\t" << " for folders minimum permissions is 700." << endl;
        os << "        " << "\t" << " Further restrictions to owner are not allowed (to avoid missfunctioning)." << endl;
        os << "        " << "\t" << " Notice that permissions of already existing files/folders will not change." << endl;
        os << "        " << "\t" << " Notice that permissions of already existing files/folders will not change." << endl;
        os << endl;
        os << "Notice: this permissions will be saved for the next time you execute MEGAcmd server. They will be removed if you logout." << endl;

    }
#endif
    else if (!strcmp(command, "https"))
    {
        os << "Shows if HTTPS is used for transfers. Use \"https on\" to enable it." << endl;
        os << endl;
        os << "HTTPS is not necesary since all data is stored and transfered encrypted." << endl;
        os << "Enabling it will increase CPU usage and add network overhead." << endl;
        os << endl;
        os << "Notice that this setting will be saved for the next time you open MEGAcmd" << endl;
    }
    else if (!strcmp(command, "deleteversions"))
    {
        os << "Deletes previous versions." << endl;
        os << endl;
        os << "This will permanently delete all historical versions of a file. " << endl;
        os << "The current version of the file will remain." << endl;
        os << "Note: any file version shared to you from a contact will need to be deleted by them." << endl;

        os << endl;
        os << "Options:" << endl;
        os << " -f   " << "\t" << "Force (no asking)" << endl;
        os << " --all" << "\t" << "Delete versions of all nodes. This will delete the version histories of all files (not current files)." << endl;
#ifdef USE_PCRE
        os << " --use-pcre" << "\t" << "use PCRE expressions" << endl;
#endif
        os << endl;
        os << "To see versions of a file use \"ls --versions\"." << endl;
        os << "To see space occupied by file versions use \"du\" with \"--versions\"." << endl;
    }
#ifdef HAVE_LIBUV
    else if (!strcmp(command, "webdav"))
    {
        os << "Configures a WEBDAV server to serve a location in MEGA" << endl;
        os << endl;
        os << "This can also be used for streaming files. The server will be running as long as MEGAcmd Server is. " << endl;
        os << "If no argument is given, it will list the webdav enabled locations." << endl;
        os << endl;
        os << "Options:" << endl;
        os << " --d        " << "\t" << "Stops serving that location" << endl;
        os << " --all      " << "\t" << "When used with -d, stops serving all locations (and stops the server)" << endl;
        os << " --public   " << "\t" << "*Allow access from outside localhost" << endl;
        os << " --port=PORT" << "\t" << "*Port to serve. DEFAULT= 4443" << endl;
        os << " --tls      " << "\t" << "*Serve with TLS (HTTPS)" << endl;
        os << " --certificate=/path/to/certificate.pem" << "\t" << "*Path to PEM formated certificate" << endl;
        os << " --key=/path/to/certificate.key" << "\t" << "*Path to PEM formated key" << endl;
#ifdef USE_PCRE
        os << " --use-pcre" << "\t" << "use PCRE expressions" << endl;
#endif
        os << endl;
        os << "*If you serve more than one location, these parameters will be ignored and use those of the first location served." << endl;
        os << " If you want to change those parameters, you need to stop serving all locations and configure them again." << endl;
        os << endl;
        os << "Caveat: This functionality is in BETA state. If you experience any issue with this, please contact: support@mega.nz" << endl;
        os << endl;
    }
    else if (!strcmp(command, "ftp"))
    {
        os << "Configures a FTP server to serve a location in MEGA" << endl;
        os << endl;
        os << "This can also be used for streaming files. The server will be running as long as MEGAcmd Server is. " << endl;
        os << "If no argument is given, it will list the ftp enabled locations." << endl;
        os << endl;
        os << "Options:" << endl;
        os << " --d        " << "\t" << "Stops serving that location" << endl;
        os << " --all      " << "\t" << "When used with -d, stops serving all locations (and stops the server)" << endl;
        os << " --public   " << "\t" << "*Allow access from outside localhost" << endl;
        os << " --port=PORT" << "\t" << "*Port to serve. DEFAULT=4990" << endl;
        os << " --data-ports=BEGIN-END" << "\t" << "*Ports range used for data channel (in passive mode). DEFAULT=1500-1600" << endl;
        os << " --tls      " << "\t" << "*Serve with TLS (FTPs)" << endl;
        os << " --certificate=/path/to/certificate.pem" << "\t" << "*Path to PEM formated certificate" << endl;
        os << " --key=/path/to/certificate.key" << "\t" << "*Path to PEM formated key" << endl;
#ifdef USE_PCRE
        os << " --use-pcre" << "\t" << "use PCRE expressions" << endl;
#endif
        os << endl;
        os << "*If you serve more than one location, these parameters will be ignored and used those of the first location served." << endl;
        os << " If you want to change those parameters, you need to stop serving all locations and configure them again." << endl;
        os << endl;
        os << "Caveat: This functionality is in BETA state. If you experience any issue with this, please contact: support@mega.nz" << endl;
        os << endl;
    }
#endif
    else if (!strcmp(command, "exclude"))
    {
        os << "Manages exclusions in syncs." << endl;
        os << endl;
        os << "Options:" << endl;
        os << " -a pattern1 pattern2 ..." << "\t" << "adds pattern(s) to the exclusion list" << endl;
        os << "                         " << "\t" << "          (* and ? wildcards allowed)" << endl;
        os << " -d pattern1 pattern2 ..." << "\t" << "deletes pattern(s) from the exclusion list" << endl;
        os << " --restart-syncs" << "\t" << "Try to restart synchronizations." << endl;

        os << endl;
        os << "Changes will not be applied immediately to actions being performed in active syncs. " << endl;
        os << "After adding/deleting patterns, you might want to: " << endl;
        os << " a) disable/reenable synchronizations manually" << endl;
        os << " b) restart MEGAcmd server" << endl;
        os << " c) use --restart-syncs flag. Caveats:" << endl;
        os << "  This will cause active transfers to be restarted" << endl;
        os << "  In certain cases --restart-syncs might be unable to re-enable a synchronization. " << endl;
        os << "  In such case, you will need to manually resume it or restart MEGAcmd server." << endl;
    }
    else if (!strcmp(command, "sync"))
    {
        os << "Controls synchronizations" << endl;
        os << endl;
        os << "If no argument is provided, it lists current configured synchronizations" << endl;
        os << endl;
        os << "If provided local and remote paths, it will start synchronizing " << endl;
        os << " a local folder into a remote folder" << endl;
        os << endl;
        os << "If an ID/local path is provided, it will list such synchronization " << endl;
        os << " unless an option is specified." << endl;
        os << endl;
        os << "Options:" << endl;
        os << "-d" << " " << "ID|localpath" << "\t" << "deletes a synchronization" << endl;
        os << "-s" << " " << "ID|localpath" << "\t" << "stops(pauses) a synchronization" << endl;
        os << "-r" << " " << "ID|localpath" << "\t" << "resumes a synchronization" << endl;
        os << " --path-display-size=N" << "\t" << "Use a fixed size of N characters for paths" << endl;
    }
    else if (!strcmp(command, "backup"))
    {
        os << "Controls backups" << endl;
        os << endl;
        os << "This command can be used to configure and control backups. " << endl;
        os << "A tutorial can be found here: https://github.com/meganz/MEGAcmd/blob/master/contrib/docs/BACKUPS.md" << endl;
        os << endl;
        os << "If no argument is given it will list the configured backups" << endl;
        os << " To get extra info on backups use -l or -h (see Options below)" << endl;
        os << endl;
        os << "When a backup of a folder (localfolder) is established in a remote folder (remotepath)" << endl;
        os << " MEGAcmd will create subfolder within the remote path with names like: \"localfoldername_bk_TIME\"" << endl;
        os << " which shall contain a backup of the local folder at that specific time" << endl;
        os << "In order to configure a backup you need to specify the local and remote paths, " << endl;
        os << "the period and max number of backups to store (see Configuration Options below)." << endl;
        os << "Once configured, you can see extended info asociated to the backup (See Display Options)" << endl;
        os << "Notice that MEGAcmd server need to be running for backups to be created." << endl;
        os << endl;
        os << "Display Options:" << endl;
        os << "-l\t" << "Show extended info: period, max number, next scheduled backup" << endl;
        os << "  \t" << " or the status of current/last backup" << endl;
        os << "-h\t" << "Show history of created backups" << endl;
        os << "  \t" << "Backup states:" << endl;
        os << "  \t"  << "While a backup is being performed, the backup will be considered and labeled as ONGOING" << endl;
        os << "  \t"  << "If a transfer is cancelled or fails, the backup will be considered INCOMPLETE" << endl;
        os << "  \t"  << "If a backup is aborted (see -a), all the transfers will be canceled and the backup be ABORTED" << endl;
        os << "  \t"  << "If MEGAcmd server stops during a transfer, it will be considered MISCARRIED" << endl;
        os << "  \t"  << "  Notice that currently when MEGAcmd server is restarted, ongoing and scheduled transfers " << endl;
        os << "  \t"  << "  will be carried out nevertheless." << endl;
        os << "  \t"  << "If MEGAcmd server is not running when a backup is scheduled and the time for the next one has already arrived," << endl;
        os << "  \t"  << " an empty BACKUP will be created with state SKIPPED" << endl;
        os << "  \t"  << "If a backup(1) is ONGOING and the time for the next backup(2) arrives, it won't start untill the previous one(1) " << endl;
        os << "  \t"  << " is completed, and if by the time the first one(1) ends the time for the next one(3) has already arrived," << endl;
        os << "  \t"  << " an empty BACKUP(2) will be created with state SKIPPED" << endl;
        os << " --path-display-size=N" << "\t" << "Use a fixed size of N characters for paths" << endl;
        printTimeFormatHelp(os);
        os << endl;
        os << "Configuration Options:" << endl;
        os << "--period=\"PERIODSTRING\"\t" << "Period: either time in TIMEFORMAT (see below) or a cron like expression" << endl;
        os << "                       \t" << " Cron like period is formatted as follows" << endl;
        os << "                       \t" << "  - - - - - -" << endl;
        os << "                       \t" << "  | | | | | |" << endl;
        os << "                       \t" << "  | | | | | |" << endl;
        os << "                       \t" << "  | | | | | +---- Day of the Week   (range: 1-7, 1 standing for Monday)" << endl;
        os << "                       \t" << "  | | | | +------ Month of the Year (range: 1-12)" << endl;
        os << "                       \t" << "  | | | +-------- Day of the Month  (range: 1-31)" << endl;
        os << "                       \t" << "  | | +---------- Hour              (range: 0-23)" << endl;
        os << "                       \t" << "  | +------------ Minute            (range: 0-59)" << endl;
        os << "                       \t" << "  +-------------- Second            (range: 0-59)" << endl;
        os << "                       \t" << " examples:" << endl;
        os << "                       \t" << "  - daily at 04:00:00 (UTC): \"0 0 4 * * *\"" << endl;
        os << "                       \t" << "  - every 15th day at 00:00:00 (UTC) \"0 0 0 15 * *\"" << endl;
        os << "                       \t" << "  - mondays at 04.30.00 (UTC): \"0 30 4 * * 1\"" << endl;
        os << "                       \t" << " TIMEFORMAT can be expressed in hours(h), days(d), " << endl;
        os << "                       \t"  << "   minutes(M), seconds(s), months(m) or years(y)" << endl;
        os << "                       \t" << "   e.g. \"1m12d3h\" indicates 1 month, 12 days and 3 hours" << endl;
        os << "                       \t" << "  Notice that this is an uncertain measure since not all months" << endl;
        os << "                       \t" << "  last the same and Daylight saving time changes are not considered" << endl;
        os << "                       \t" << "  If possible use a cron like expresion" << endl;
        os << "                       \t" << "Notice: regardless of the period expresion, the first time you establish a backup," << endl;
        os << "                       \t" << " it will be created immediately" << endl;
        os << "--num-backups=N\t" << "Maximum number of backups to store" << endl;
        os << "                 \t" << " After creating the backup (N+1) the oldest one will be deleted" << endl;
        os << "                 \t" << "  That might not be true in case there are incomplete backups:" << endl;
        os << "                 \t" << "   in order not to lose data, at least one COMPLETE backup will be kept" << endl;
        os << "Use backup TAG|localpath --option=VALUE to modify existing backups" << endl;
        os << endl;
        os << "Management Options:" << endl;
        os << "-d TAG|localpath\t" << "Removes a backup by its TAG or local path" << endl;
        os << "                \t" << " Folders created by backup won't be deleted" << endl;
        os << "-a TAG|localpath\t" << "Aborts ongoing backup" << endl;
        os << endl;
        os << "Caveat: This functionality is in BETA state. If you experience any issue with this, please contact: support@mega.nz" << endl;
    }
    else if (!strcmp(command, "export"))
    {
        os << "Prints/Modifies the status of current exports" << endl;
        os << endl;
        os << "Options:" << endl;
#ifdef USE_PCRE
        os << " --use-pcre" << "\t" << "use PCRE expressions" << endl;
#endif
        os << " -a" << "\t" << "Adds an export (or modifies it if existing)" << endl;
        os << " --password=PASSWORD" << "\t" << "Protects link with password." << endl;
        os << " --expire=TIMEDELAY" << "\t" << "Determines the expiration time of a node." << endl;
        os << "                   " << "\t" << "   It indicates the delay in hours(h), days(d), " << endl;
        os << "                   " << "\t"  << "   minutes(M), seconds(s), months(m) or years(y)" << endl;
        os << "                   " << "\t" << "   e.g. \"1m12d3h\" establish an expiration time 1 month, " << endl;
        os << "                   " << "\t"  << "   12 days and 3 hours after the current moment" << endl;
        os << " -f" << "\t" << "Implicitly accept copyright terms (only shown the first time an export is made)" << endl;
        os << "   " << "\t" << "MEGA respects the copyrights of others and requires that users of the MEGA cloud service " << endl;
        os << "   " << "\t" << "comply with the laws of copyright." << endl;
        os << "   " << "\t" << "You are strictly prohibited from using the MEGA cloud service to infringe copyrights." << endl;
        os << "   " << "\t" << "You may not upload, download, store, share, display, stream, distribute, email, link to, " << endl;
        os << "   " << "\t" << "transmit or otherwise make available any files, data or content that infringes any copyright " << endl;
        os << "   " << "\t" << "or other proprietary rights of any person or entity." << endl;
        os << " -d" << "\t" << "Deletes an export" << endl;
        printTimeFormatHelp(os);
        os << endl;
        os << "If a remote path is given it'll be used to add/delete or in case of no option selected," << endl;
        os << " it will display all the exports existing in the tree of that path" << endl;
    }
    else if (!strcmp(command, "share"))
    {
        os << "Prints/Modifies the status of current shares" << endl;
        os << endl;
        os << "Options:" << endl;
#ifdef USE_PCRE
        os << " --use-pcre" << "\t" << "use PCRE expressions" << endl;
#endif
        os << " -p" << "\t" << "Show pending shares" << endl;
        os << " --with=email" << "\t" << "Determines the email of the user to [no longer] share with" << endl;
        os << " -d" << "\t" << "Stop sharing with the selected user" << endl;
        os << " -a" << "\t" << "Adds a share (or modifies it if existing)" << endl;
        os << " --level=LEVEL" << "\t" << "Level of acces given to the user" << endl;
        os << "              " << "\t" << "0: " << "Read access" << endl;
        os << "              " << "\t" << "1: " << "Read and write" << endl;
        os << "              " << "\t" << "2: " << "Full access" << endl;
        os << "              " << "\t" << "3: " << "Owner access" << endl;
        os << endl;
        os << "If a remote path is given it'll be used to add/delete or in case " << endl;
        os << " of no option selected, it will display all the shares existing " << endl;
        os << " in the tree of that path" << endl;
        os << endl;
        os << "When sharing a folder with a user that is not a contact (see \"users --help\")" << endl;
        os << "  the share will be in a pending state. You can list pending shares with" << endl;
        os << " \"share -p\". He would need to accept your invitation (see \"ipc\")" << endl;
        os << endl;
        os << "If someone has shared something with you, it will be listed as a root folder" << endl;
        os << " Use \"mount\" to list folders shared with you" << endl;
    }
    else if (!strcmp(command, "invite"))
    {
        os << "Invites a contact / deletes an invitation" << endl;
        os << endl;
        os << "Options:" << endl;
        os << " -d" << "\t" << "Deletes invitation" << endl;
        os << " -r" << "\t" << "Sends the invitation again" << endl;
        os << " --message=\"MESSAGE\"" << "\t" << "Sends inviting message" << endl;
        os << endl;
        os << "Use \"showpcr\" to browse invitations" << endl;
        os << "Use \"ipc\" to manage invitations received" << endl;
        os << "Use \"users\" to see contacts" << endl;
    }
    if (!strcmp(command, "ipc"))
    {
        os << "Manages contact incoming invitations." << endl;
        os << endl;
        os << "Options:" << endl;
        os << " -a" << "\t" << "Accepts invitation" << endl;
        os << " -d" << "\t" << "Rejects invitation" << endl;
        os << " -i" << "\t" << "Ignores invitation [WARNING: do not use unless you know what you are doing]" << endl;
        os << endl;
        os << "Use \"invite\" to send/remove invitations to other users" << endl;
        os << "Use \"showpcr\" to browse incoming/outgoing invitations" << endl;
        os << "Use \"users\" to see contacts" << endl;
    }
    if (!strcmp(command, "masterkey"))
    {
        os << "Shows your master key." << endl;
        os << endl;
        os << "Your data is only readable through a chain of decryption operations that begins " << endl;
        os << "with your master encryption key (Recovery Key), which MEGA stores encrypted with your password." << endl;
        os << "This means that if you lose your password, your Recovery Key can no longer be decrypted, " << endl;
        os << "and you can no longer decrypt your data." << endl;
        os << "Exporting the Recovery Key and keeping it in a secure location " << endl;
        os << "enables you to set a new password without data loss." << endl;
        os << "Always keep physical control of your master key (e.g. on a client device, external storage, or print)" << endl;
    }
    if (!strcmp(command, "showpcr"))
    {
        os << "Shows incoming and outgoing contact requests." << endl;
        os << endl;
        os << "Options:" << endl;
        os << " --in" << "\t" << "Shows incoming requests" << endl;
        os << " --out" << "\t" << "Shows outgoing invitations" << endl;
        printTimeFormatHelp(os);
        os << endl;
        os << "Use \"ipc\" to manage invitations received" << endl;
        os << "Use \"users\" to see contacts" << endl;
    }
    else if (!strcmp(command, "users"))
    {
        os << "List contacts" << endl;
        os << endl;
        os << "Options:" << endl;
        os << " -s" << "\t" << "Show shared folders with listed contacts" << endl;
        os << " -h" << "\t" << "Show all contacts (hidden, blocked, ...)" << endl;
        os << " -n" << "\t" << "Show users names" << endl;
        os << " -d" << "\tcontact@email " << "Deletes the specified contact" << endl;
        printTimeFormatHelp(os);
        os << endl;
        os << "Use \"invite\" to send/remove invitations to other users" << endl;
        os << "Use \"showpcr\" to browse incoming/outgoing invitations" << endl;
        os << "Use \"ipc\" to manage invitations received" << endl;
        os << "Use \"users\" to see contacts" << endl;
    }
    else if (!strcmp(command, "speedlimit"))
    {
        os << "Displays/modifies upload/download rate limits" << endl;
        os << " NEWLIMIT establish the new limit in size per second (0 = no limit)" << endl;
        os << " NEWLIMIT may include (B)ytes, (K)ilobytes, (M)egabytes, (G)igabytes & (T)erabytes." << endl;
        os << "  Examples: \"1m12k3B\" \"3M\". If no units are given, bytes are assumed" << endl;
        os << endl;
        os << "Options:" << endl;
        os << " -d" << "\t" << "Download speed limit" << endl;
        os << " -u" << "\t" << "Upload speed limit" << endl;
        os << " -h" << "\t" << "Human readable" << endl;
        os << endl;
        os << "Notice: this limit will be saved for the next time you execute MEGAcmd server. They will be removed if you logout." << endl;
    }
    else if (!strcmp(command, "killsession"))
    {
        os << "Kills a session of current user." << endl;
        os << endl;
        os << "Options:" << endl;
        os << " -a" << "\t" << "kills all sessions except the current one" << endl;
        os << endl;
        os << "To see all sessions use \"whoami -l\"" << endl;
    }
    else if (!strcmp(command, "whoami"))
    {
        os << "Prints info of the user" << endl;
        os << endl;
        os << "Options:" << endl;
        os << " -l" << "\t" << "Show extended info: total storage used, storage per main folder " << endl;
        os << "   " << "\t" << "(see mount), pro level, account balance, and also the active sessions" << endl;
    }
    else if (!strcmp(command, "df"))
    {
        os << "Shows storage info" << endl;
        os << endl;
        os << "Shows total storage used in the account, storage per main folder (see mount)" << endl;
        os << endl;
        os << "Options:" << endl;
        os << " -h" << "\t" << "Human readable sizes. Otherwise, size will be expressed in Bytes" << endl;
    }
    else if (!strcmp(command, "cat"))
    {
        os << "Prints the contents of remote files" << endl;
        os << endl;
#ifdef _WIN32
        os << "To avoid issues with encoding, if you want to cat the exact binary contents of a remote file into a local one, " << endl;
        os << "use non-interactive mode with -o /path/to/file. See help \"non-interactive\"" << endl;
#endif
    }
    else if (!strcmp(command, "mediainfo"))
    {
        os << "Prints media info of remote files" << endl;
        os << endl;
        os << "Options:" << endl;
        os << " --path-display-size=N" << "\t" << "Use a fixed size of N characters for paths" << endl;
    }
    else if (!strcmp(command, "passwd"))
    {
        os << "Modifies user password" << endl;
        os << endl;
        os << "Notice that modifying the password will close all your active sessions" << endl;
        os << " in all your devices (except for the current one)" << endl;
        os << endl;
        os << "Options:" << endl;
        os << " -f   " << "\t" << "Force (no asking)" << endl;
        os << " --auth-code=XXXX" << "\t" << "Two-factor Authentication code. More info: https://mega.nz/blog_48" << endl;
    }
    else if (!strcmp(command, "reload"))
    {
        os << "Forces a reload of the remote files of the user" << endl;
        os << "It will also resume synchronizations." << endl;
    }
    else if (!strcmp(command, "version"))
    {
        os << "Prints MEGAcmd versioning and extra info" << endl;
        os << endl;
        os << "Options:" << endl;
        os << " -c" << "\t" << "Shows changelog for the current version" << endl;
        os << " -l" << "\t" << "Show extended info: MEGA SDK version and features enabled" << endl;
    }
    else if (!strcmp(command, "thumbnail"))
    {
        os << "To download/upload the thumbnail of a file." << endl;
        os << " If no -s is inidicated, it will download the thumbnail." << endl;
        os << endl;
        os << "Options:" << endl;
        os << " -s" << "\t" << "Sets the thumbnail to the specified file" << endl;
    }
    else if (!strcmp(command, "preview"))
    {
        os << "To download/upload the preview of a file." << endl;
        os << " If no -s is inidicated, it will download the preview." << endl;
        os << endl;
        os << "Options:" << endl;
        os << " -s" << "\t" << "Sets the preview to the specified file" << endl;
    }
    else if (!strcmp(command, "find"))
    {
        os << "Find nodes matching a pattern" << endl;
        os << endl;
        os << "Options:" << endl;
        os << " --pattern=PATTERN" << "\t" << "Pattern to match";
        os << " (" << getsupportedregexps() << ") " << endl;
        os << " --mtime=TIMECONSTRAIN" << "\t" << "Determines time constrains, in the form: [+-]TIMEVALUE" << endl;
        os << "                      " << "\t" << "  TIMEVALUE may include hours(h), days(d), minutes(M)," << endl;
        os << "                      " << "\t" << "   seconds(s), months(m) or years(y)" << endl;
        os << "                      " << "\t" << "  Examples:" << endl;
        os << "                      " << "\t" << "   \"+1m12d3h\" shows files modified before 1 month, " << endl;
        os << "                      " << "\t" << "    12 days and 3 hours the current moment" << endl;
        os << "                      " << "\t" << "   \"-3h\" shows files modified within the last 3 hours" << endl;
        os << "                      " << "\t" << "   \"-3d+1h\" shows files modified in the last 3 days prior to the last hour" << endl;
        os << " --size=SIZECONSTRAIN" << "\t" << "Determines size constrains, in the form: [+-]TIMEVALUE" << endl;
        os << "                      " << "\t" << "  TIMEVALUE may include (B)ytes, (K)ilobytes, (M)egabytes, (G)igabytes & (T)erabytes" << endl;
        os << "                      " << "\t" << "  Examples:" << endl;
        os << "                      " << "\t" << "   \"+1m12k3B\" shows files bigger than 1 Mega, 12 Kbytes and 3Bytes" << endl;
        os << "                      " << "\t" << "   \"-3M\" shows files smaller than 3 Megabytes" << endl;
        os << "                      " << "\t" << "   \"-4M+100K\" shows files smaller than 4 Mbytes and bigger than 100 Kbytes" << endl;
        os << " --show-handles" << "\t" << "Prints files/folders handles (H:XXXXXXXX). You can address a file/folder by its handle" << endl;
#ifdef USE_PCRE
        os << " --use-pcre" << "\t" << "use PCRE expressions" << endl;
#endif
        os << " -l" << "\t" << "Prints file info" << endl;
        printTimeFormatHelp(os);
    }
    else if(!strcmp(command,"debug") )
    {
        os << "Enters debugging mode (HIGHLY VERBOSE)" << endl;
        os << endl;
        os << "For a finer control of log level see \"log --help\"" << endl;
    }
    else if (!strcmp(command, "quit") || !strcmp(command, "exit"))
    {
        os << "Quits MEGAcmd" << endl;
        os << endl;
        os << "Notice that the session will still be active, and local caches available" << endl;
        os << "The session will be resumed when the service is restarted" << endl;
        if (getCurrentThreadIsCmdShell())
        {
            os << endl;
            os << "Be aware that this will exit both the interactive shell and the server." << endl;
            os << "To only exit current shell and keep server running, use \"exit --only-shell\"" << endl;
        }
    }
    else if (!strcmp(command, "transfers"))
    {
        os << "List or operate with transfers" << endl;
        os << endl;
        os << "If executed without option it will list the first 10 tranfers" << endl;
        os << "Options:" << endl;
        os << " -c (TAG|-a)" << "\t" << "Cancel transfer with TAG (or all with -a)" << endl;
        os << " -p (TAG|-a)" << "\t" << "Pause transfer with TAG (or all with -a)" << endl;
        os << " -r (TAG|-a)" << "\t" << "Resume transfer with TAG (or all with -a)" << endl;
        os << " --only-uploads" << "\t" << "Show/Operate only upload transfers" << endl;
        os << " --only-downloads" << "\t" << "Show/Operate only download transfers" << endl;
        os << endl;
        os << "Show options:" << endl;
        os << " --summary" << "\t" << "Prints summary of on going transfers" << endl;
        os << " --show-syncs" << "\t" << "Show synchronization transfers" << endl;
        os << " --show-completed" << "\t" << "Show completed transfers" << endl;
        os << " --only-completed" << "\t" << "Show only completed download" << endl;
        os << " --limit=N" << "\t" << "Show only first N transfers" << endl;
        os << " --path-display-size=N" << "\t" << "Use a fixed size of N characters for paths" << endl;
        os << endl;
        os << "TYPE legend correspondence:" << endl;
#ifdef _WIN32
        os << "  D = \t" << "Download transfer" << endl;
        os << "  U = \t" << "Upload transfer" << endl;
        os << "  S = \t" << "Sync transfer. The transfer is done in the context of a synchronization" << endl;
        os << "  B = \t" << "Backup transfer. The transfer is done in the context of a backup" << endl;
#else
        os << "  \u21d3 = \t" << "Download transfer" << endl;
        os << "  \u21d1 = \t" << "Upload transfer" << endl;
        os << "  \u21f5 = \t" << "Sync transfer. The transfer is done in the context of a synchronization" << endl;
        os << "  \u23eb = \t" << "Backup transfer. The transfer is done in the context of a backup" << endl;
#endif
    }
#if defined(_WIN32) && defined(NO_READLINE)
    else if (!strcmp(command, "autocomplete"))
    {
        os << "Modifes how tab completion operates." << endl;
        os << endl;
        os << "The default is to operate like the native platform. However" << endl;
        os << "you can switch it between mode 'dos' and 'unix' as you prefer." << endl;
        os << "Options:" << endl;
        os << " dos" << "\t" << "Each press of tab places the next option into the command line" << endl;
        os << " unix" << "\t" << "Options are listed in a table, or put in-line if there is only one" << endl;
    }
    else if (!strcmp(command, "codepage"))
    {
        os << "Switches the codepage used to decide which characters show on-screen." << endl;
        os << endl;
        os << "MEGAcmd supports unicode or specific code pages.  For european countries you may need" << endl;
        os << "to select a suitable codepage or secondary codepage for the character set you use." << endl;
        os << "Of course a font containing the glyphs you need must have been selected for the terminal first." << endl;
        os << "Options:" << endl;
        os << " (no option)" << "\t" << "Outputs the selected code page and secondary codepage (if configured)." << endl;
        os << " N" << "\t" << "Sets the main codepage to N. 65001 is Unicode." << endl;
        os << " M" << "\t" << "Sets the secondary codepage to M, which is used if the primary can't translate a character." << endl;
    }
#endif
    return os.str();
}

#define SSTR( x ) static_cast< const std::ostringstream & >( \
        ( std::ostringstream() << std::dec << x ) ).str()

void printAvailableCommands(int extensive = 0)
{
    vector<string> validCommandsOrdered = validCommands;
    sort(validCommandsOrdered.begin(), validCommandsOrdered.end());
    if (!extensive)
    {
        size_t i = 0;
        size_t j = (validCommandsOrdered.size()/3)+((validCommandsOrdered.size()%3>0)?1:0);
        size_t k = 2*(validCommandsOrdered.size()/3)+validCommandsOrdered.size()%3;
        for (i = 0; i < validCommandsOrdered.size() && j < validCommandsOrdered.size()  && k < validCommandsOrdered.size(); i++, j++, k++)
        {
            OUTSTREAM << "      " << getLeftAlignedStr(validCommandsOrdered.at(i), 20) <<  getLeftAlignedStr(validCommandsOrdered.at(j), 20)  <<  "      " << validCommandsOrdered.at(k) << endl;
        }
        if (validCommandsOrdered.size()%3)
        {
            OUTSTREAM << "      " << getLeftAlignedStr(validCommandsOrdered.at(i), 20) ;
            if (validCommandsOrdered.size()%3 > 1 )
            {
                OUTSTREAM << getLeftAlignedStr(validCommandsOrdered.at(j), 20) ;
            }
            OUTSTREAM << endl;
        }
    }
    else
    {
        for (size_t i = 0; i < validCommandsOrdered.size(); i++)
        {
            if (validCommandsOrdered.at(i)!="completion")
            {
                if (extensive > 1)
                {
                    unsigned int width = getNumberOfCols();

                    OUTSTREAM <<  "<" << validCommandsOrdered.at(i) << ">" << endl;
                    OUTSTREAM <<  getHelpStr(validCommandsOrdered.at(i).c_str());
                    for (unsigned int j = 0; j< width; j++) OUTSTREAM << "-";
                    OUTSTREAM << endl;
                }
                else
                {
                    OUTSTREAM << "      " << getUsageStr(validCommandsOrdered.at(i).c_str());
                    string helpstr = getHelpStr(validCommandsOrdered.at(i).c_str());
                    helpstr=string(helpstr,helpstr.find_first_of("\n")+1);
                    OUTSTREAM << ": " << string(helpstr,0,helpstr.find_first_of("\n"));

                    OUTSTREAM << endl;
                }
            }
        }
    }
}

void executecommand(char* ptr)
{
    vector<string> words = getlistOfWords(ptr, !getCurrentThreadIsCmdShell());
    if (!words.size())
    {
        return;
    }

    string thecommand = words[0];

    if (( thecommand == "?" ) || ( thecommand == "h" ))
    {
        printAvailableCommands();
        return;
    }

    if (words[0] == "completion")
    {
        if (words.size() < 3) words.push_back("");
        vector<string> wordstocomplete(words.begin()+1,words.end());
        setCurrentThreadLine(wordstocomplete);
        OUTSTREAM << getListOfCompletionValues(wordstocomplete);
        return;
    }
    if (words[0] == "retrycons")
    {
        api->retryPendingConnections();
        return;
    }
    if (words[0] == "loggedin")
    {
        if (!api->isFilesystemAvailable())
        {
            setCurrentOutCode(MCMD_NOTLOGGEDIN);
        }
        return;
    }
    if (words[0] == "completionshell")
    {
        if (words.size() == 2)
        {
            vector<string> validCommandsOrdered = validCommands;
            sort(validCommandsOrdered.begin(), validCommandsOrdered.end());
            for (size_t i = 0; i < validCommandsOrdered.size(); i++)
            {
                if (validCommandsOrdered.at(i)!="completion")
                {
                    OUTSTREAM << validCommandsOrdered.at(i);
                    if (i != validCommandsOrdered.size() -1)
                    {
                        OUTSTREAM << (char)0x1F;
                    }
                }
            }
        }
        else
        {
            if (words.size() < 3) words.push_back("");
            vector<string> wordstocomplete(words.begin()+1,words.end());
            setCurrentThreadLine(wordstocomplete);
            OUTSTREAM << getListOfCompletionValues(wordstocomplete,(char)0x1F, string().append(1, (char)0x1F).c_str(), false);
        }

        return;
    }

    words = getlistOfWords(ptr, !getCurrentThreadIsCmdShell(), true); //Get words again ignoring trailing spaces (only reasonable for completion)

    map<string, string> cloptions;
    map<string, int> clflags;

    set<string> validParams;
    addGlobalFlags(&validParams);

    if (setOptionsAndFlags(&cloptions, &clflags, &words, validParams, true))
    {
        setCurrentOutCode(MCMD_EARGS);
        LOG_err << "      " << getUsageStr(thecommand.c_str());
        return;
    }

    insertValidParamsPerCommand(&validParams, thecommand);

    if (!validCommand(thecommand))   //unknown command
    {
        setCurrentOutCode(MCMD_EARGS);
        LOG_err << "Command not found: " << thecommand;
        return;
    }

    if (setOptionsAndFlags(&cloptions, &clflags, &words, validParams))
    {
        setCurrentOutCode(MCMD_EARGS);
        LOG_err << "      " << getUsageStr(thecommand.c_str());
        return;
    }
    setCurrentThreadLogLevel(MegaApi::LOG_LEVEL_ERROR + (getFlag(&clflags, "v")?(1+getFlag(&clflags, "v")):0));

    if (getFlag(&clflags, "help"))
    {
        string h = getHelpStr(thecommand.c_str());
        OUTSTREAM << h << endl;
        return;
    }

    if ( thecommand == "help" )
    {
        if (getFlag(&clflags,"upgrade"))
        {

             const char *userAgent = api->getUserAgent();
             char* url = new char[strlen(userAgent)+10];

             sprintf(url, "pro/uao=%s",userAgent);

             string theurl;

             if (api->isLoggedIn())
             {
                 MegaCmdListener *megaCmdListener = new MegaCmdListener(api, NULL);
                 api->getSessionTransferURL(url, megaCmdListener);
                 megaCmdListener->wait();
                 if (megaCmdListener->getError() && megaCmdListener->getError()->getErrorCode() == MegaError::API_OK)
                 {
                     theurl = megaCmdListener->getRequest()->getLink();
                 }
                 else
                 {
                     setCurrentOutCode(MCMD_EUNEXPECTED);
                     LOG_warn << "Unable to get session transfer url: " << megaCmdListener->getError()->getErrorString();
                 }
                 delete megaCmdListener;
             }

             if (!theurl.size())
             {
                 theurl = "https://mega.nz/pro";
             }

             OUTSTREAM << "MEGA offers different PRO plans to increase your allowed transfer quota and user storage." << endl;
             OUTSTREAM << "Open the following link in your browser to obtain a PRO account: " << endl;
             OUTSTREAM << "  " << theurl << endl;

             delete [] url;
        }
        else if (getFlag(&clflags,"non-interactive"))
        {
            OUTSTREAM << "MEGAcmd features two modes of interaction:" << endl;
            OUTSTREAM << " - interactive: entering commands in this shell. Enter \"help\" to list available commands" << endl;
            OUTSTREAM << " - non-interactive: MEGAcmd is also listening to outside petitions" << endl;
            OUTSTREAM << "For the non-interactive mode, there are client commands you can use. " << endl;
#ifdef _WIN32

            OUTSTREAM << "Along with the interactive shell, there should be several mega-*.bat scripts" << endl;
            OUTSTREAM << "installed with MEGAcmd. You can use them writting their absolute paths, " << endl;
            OUTSTREAM << "or including their location into your environment PATH and execute simply with mega-*" << endl;
            OUTSTREAM << "If you use PowerShell, you can add the the location of the scripts to the PATH with:" << endl;
            OUTSTREAM << "  $env:PATH += \";$env:LOCALAPPDATA\\MEGAcmd\"" << endl;
            OUTSTREAM << "Client commands completion requires bash, hence, it is not available for Windows. " << endl;
            OUTSTREAM << "You can add \" -o outputfile\" to save the output into a file instead of to standard output." << endl;
            OUTSTREAM << endl;

#elif __MACH__
            OUTSTREAM << "After installing the dmg, along with the interactive shell, client commands" << endl;
            OUTSTREAM << "should be located at /Applications/MEGAcmd.app/Contents/MacOS" << endl;
            OUTSTREAM << "If you wish to use the client commands from MacOS Terminal, open the Terminal and " << endl;
            OUTSTREAM << "include the installation folder in the PATH. Typically:" << endl;
            OUTSTREAM << endl;
            OUTSTREAM << " export PATH=/Applications/MEGAcmd.app/Contents/MacOS:$PATH" << endl;
            OUTSTREAM << endl;
            OUTSTREAM << "And for bash completion, source megacmd_completion.sh:" << endl;
            OUTSTREAM << " source /Applications/MEGAcmd.app/Contents/MacOS/megacmd_completion.sh" << endl;
#else
            OUTSTREAM << "If you have installed MEGAcmd using one of the available packages" << endl;
            OUTSTREAM << "both the interactive shell (mega-cmd) and the different client commands (mega-*) " << endl;
            OUTSTREAM << "will be in your PATH (you might need to open your shell again). " << endl;
            OUTSTREAM << "If you are using bash, you should also have autocompletion for client commands working. " << endl;

#endif
        }

#if defined(_WIN32) && defined(NO_READLINE)
        else if (getFlag(&clflags, "unicode"))
        {
            OUTSTREAM << "Unicode support has been considerably improved in the interactive console since version 1.0.0." << endl;
            OUTSTREAM << "If you do experience issues with it, please do not hesistate to contact us." << endl;
            OUTSTREAM << endl;
            OUTSTREAM << "Known issues: " << endl;
            OUTSTREAM << endl;
            OUTSTREAM << "If some symbols are not displaying, or displaying correctly, please first check you have a suitable font" << endl;
            OUTSTREAM << "selected, and a suitable codepage. See \"help codepage\" for details on that." << endl;
            OUTSTREAM << "When using the non-interactive mode (See \"help --non-interactive\"), piping or redirecting can be quite" << endl;
            OUTSTREAM << "problematic due to different encoding expectations between programs.  You can use \"-o outputfile\" with your " << endl;
            OUTSTREAM << "mega-*.bat commands to have the output written to a file in UTF-8, and then open it with a suitable editor." << endl;
        }
#elif defined(_WIN32)
        else if (getFlag(&clflags,"unicode"))
        {
            OUTSTREAM << "A great effort has been done so as to have MEGAcmd support non-ASCII characters." << endl;
            OUTSTREAM << "However, it might still be consider in an experimantal state. You might experiment some issues." << endl;
            OUTSTREAM << "If that is the case, do not hesistate to contact us so as to improve our support." << endl;
            OUTSTREAM << endl;
            OUTSTREAM << "Known issues: " << endl;
            OUTSTREAM << endl;
            OUTSTREAM << "In Windows, when executing a client command in non-interactive mode or the interactive shell " << endl;
            OUTSTREAM << "Some symbols might not be printed. This is something expected, since your terminal (PowerShell/Command Prompt)" << endl;
            OUTSTREAM << "is not able to draw those symbols. However you can use the non-interactive mode to have the output " << endl;
            OUTSTREAM << "written into a file and open it with a graphic editor that supports them. The file will be UTF-8 encoded." << endl;
            OUTSTREAM << "To do that, use \"-o outputfile\" with your mega-*.bat commands. (See \"help --non-interactive\")." << endl;
            OUTSTREAM << "Please, restrain using \"> outputfile\" or piping the output into another command if you require unicode support" << endl;
            OUTSTREAM << "because for instance, when piping, your terminal does not treat the output as binary; " << endl;
            OUTSTREAM << "it will meddle with the encoding, resulting in unusable output." << endl;
            OUTSTREAM << endl;
            OUTSTREAM << "In the interactive shell, the library used for reading the inputs is not able to capture unicode inputs by default" << endl;
            OUTSTREAM << "There's a workaround to activate an alternative way to read input. You can activate it using \"unicode\" command. " << endl;
            OUTSTREAM << "However, if you do so, arrow keys and hotkeys combinations will be disabled. You can disable this input mode again. " << endl;
            OUTSTREAM << "See \"unicode --help\" for further info." << endl;
        }
#endif
        else
        {
            OUTSTREAM << "Here is the list of available commands and their usage" << endl;
            OUTSTREAM << "Use \"help -f\" to get a brief description of the commands" << endl;
            OUTSTREAM << "You can get further help on a specific command with \"command\" --help " << endl;
            OUTSTREAM << "Alternatively, you can use \"help\" -ff to get a complete description of all commands" << endl;
            OUTSTREAM << "Use \"help --non-interactive\" to learn how to use MEGAcmd with scripts" << endl;
            OUTSTREAM << "Use \"help --upgrade\" to learn about the limitations and obtaining PRO accounts" << endl;

            OUTSTREAM << endl << "Commands:" << endl;

            printAvailableCommands(getFlag(&clflags,"f"));
            OUTSTREAM << endl << "Verbosity: You can increase the amount of information given by any command by passing \"-v\" (\"-vv\", \"-vvv\", ...)" << endl;
        }
        return;
    }

    cmdexecuter->executecommand(words, &clflags, &cloptions);
}

bool executeUpdater(bool *restartRequired, bool doNotInstall = false)
{
    LOG_debug << "Executing updater..." ;
#ifdef _WIN32

#ifndef NDEBUG
    LPCWSTR szPath = TEXT("..\\MEGAcmdUpdater\\debug\\MEGAcmdUpdater.exe");
#else
    TCHAR szPath[MAX_PATH];

    if (!SUCCEEDED(GetModuleFileName(NULL, szPath , MAX_PATH)))
    {
        LOG_err << "Couldnt get EXECUTABLE folder: " << wstring(szPath);
        setCurrentOutCode(MCMD_EUNEXPECTED);
        return false;
    }

    if (SUCCEEDED(PathRemoveFileSpec(szPath)))
    {
        if (!PathAppend(szPath,TEXT("MEGAcmdUpdater.exe")))
        {
            LOG_err << "Couldnt append MEGAcmdUpdater exec: " << wstring(szPath);
            setCurrentOutCode(MCMD_EUNEXPECTED);
            return false;
        }
    }
    else
    {
        LOG_err << "Couldnt remove file spec: " << wstring(szPath);
        setCurrentOutCode(MCMD_EUNEXPECTED);
        return false;
    }
#endif
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    ZeroMemory( &si, sizeof(si) );
    ZeroMemory( &pi, sizeof(pi) );

    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    TCHAR szPathUpdaterCL[MAX_PATH+30];
    if (doNotInstall)
    {
        wsprintfW(szPathUpdaterCL,L"%ls --normal-update --do-not-install", szPath);
    }
    else
    {
        wsprintfW(szPathUpdaterCL,L"%ls --normal-update", szPath);
    }
    LOG_verbose << "Executing: " << wstring(szPathUpdaterCL);
    if (!CreateProcess( szPath,(LPWSTR) szPathUpdaterCL,NULL,NULL,TRUE,
                        0,
                        NULL,NULL,
                        &si,&pi) )
    {
        LOG_err << "Unable to execute: <" << wstring(szPath) << "> errno = : " << ERRNO;
        setCurrentOutCode(MCMD_EUNEXPECTED);
        return false;
    }

    WaitForSingleObject( pi.hProcess, INFINITE );

    DWORD exit_code;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    *restartRequired = exit_code != 0;

    LOG_verbose << " The execution of Updater returns: " << exit_code;

    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );

#else
    pid_t pidupdater = fork();

    if ( pidupdater == 0 )
    {
        char * donotinstallstr = NULL;
        if (doNotInstall)
        {
            donotinstallstr = "--do-not-install";
        }

#ifdef __MACH__
#ifndef NDEBUG
        char * args[] = {"../../../../MEGAcmdUpdater/MEGAcmdUpdater.app/Contents/MacOS/MEGAcmdUpdater", "--normal-update", donotinstallstr, NULL};
#else
        char * args[] = {"/Applications/MEGAcmd.app/Contents/MacOS/MEGAcmdUpdater", "--normal-update", donotinstallstr, NULL};
#endif
#else //linux don't use autoupdater: this is just for testing
#ifndef NDEBUG
        char * args[] = {"../MEGAcmdUpdater/MEGAcmdUpdater", "--normal-update", donotinstallstr, NULL}; // notice: won't work after lcd
#else
        char * args[] = {"mega-cmd-updater", "--normal-update", donotinstallstr, NULL};
#endif
#endif

        LOG_verbose << "Exec updater line: " << args[0] << " " << args[1] << " " << args[2];

        if (execvp(args[0], args) < 0)
        {

            LOG_err << " FAILED to initiate updater. errno = " << ERRNO;
        }
    }

    int status;

    waitpid(pidupdater, &status, 0);

    if ( WIFEXITED(status) )
    {
        int exit_code = WEXITSTATUS(status);
        LOG_debug << "Exit status of the updater was " << exit_code;
        *restartRequired = exit_code != 0;

    }
    else
    {
        LOG_err << " Unexpected error waiting for Updater. errno = " << ERRNO;
    }
#endif
    return true;
}

bool restartServer()
{
#ifdef _WIN32
        LPWSTR szPathExecQuoted = GetCommandLineW();
        wstring wspathexec = wstring(szPathExecQuoted);

        if (wspathexec.at(0) == '"')
        {
            wspathexec = wspathexec.substr(1);
        }

        size_t pos = wspathexec.find(L"--wait-for");
        if (pos != string::npos)
        {
            wspathexec = wspathexec.substr(0,pos);
        }

        while (wspathexec.size() && ( wspathexec.at(wspathexec.size()-1) == '"' || wspathexec.at(wspathexec.size()-1) == ' ' ))
        {
            wspathexec = wspathexec.substr(0,wspathexec.size()-1);
        }

        LPWSTR szPathServerCommand = (LPWSTR) wspathexec.c_str();
        TCHAR szPathServer[MAX_PATH];
        if (!SUCCEEDED(GetModuleFileName(NULL, szPathServer , MAX_PATH)))
        {
            LOG_err << "Couldnt get EXECUTABLE folder: " << wstring(szPathServer);
            setCurrentOutCode(MCMD_EUNEXPECTED);
            return false;
        }

        LOG_debug << "Restarting the server : <" << wstring(szPathServerCommand) << ">";

        STARTUPINFO si;
        PROCESS_INFORMATION pi;
        ZeroMemory( &si, sizeof(si) );
        ZeroMemory( &pi, sizeof(pi) );
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        TCHAR szPathServerCL[MAX_PATH+30];
        wsprintfW(szPathServerCL,L"%ls --wait-for %d", szPathServerCommand, GetCurrentProcessId());
        LOG_verbose  << "Executing: " << wstring(szPathServerCL);
        if (!CreateProcess( szPathServer,(LPWSTR) szPathServerCL,NULL,NULL,TRUE,
                            0,
                            NULL,NULL,
                            &si,&pi) )
        {
            LOG_debug << "Unable to execute: <" << wstring(szPathServerCL) << "> errno = : " << ERRNO;
            return false;
        }
#else
    pid_t childid = fork();
    if ( childid ) //parent
    {
        char **argv = new char*[mcmdMainArgc+3];
        int i = 0;
        for (;i < mcmdMainArgc; i++)
        {
            argv[i]=mcmdMainArgv[i];
        }

        argv[i++]="--wait-for";
        argv[i++]=(char*)SSTR(childid).c_str();
        argv[i++]=NULL;
        LOG_debug << "Restarting the server : <" << argv[0] << ">";

        execv(argv[0],argv);
    }
#endif


    LOG_debug << "Server restarted, indicating the shell to restart also";
    setCurrentOutCode(MCMD_REQRESTART);

    string s = "restart";
    cm->informStateListeners(s);

    return true;
}

static bool process_line(char* l)
{
    switch (prompt)
    {
        case AREYOUSURETODELETE:
            if (!strcmp(l,"yes") || !strcmp(l,"YES") || !strcmp(l,"y") || !strcmp(l,"Y"))
            {
                cmdexecuter->confirmDelete();
            }
            else if (!strcmp(l,"no") || !strcmp(l,"NO") || !strcmp(l,"n") || !strcmp(l,"N"))
            {
                cmdexecuter->discardDelete();
            }
            else if (!strcmp(l,"All") || !strcmp(l,"ALL") || !strcmp(l,"a") || !strcmp(l,"A") || !strcmp(l,"all"))
            {
                cmdexecuter->confirmDeleteAll();
            }
            else if (!strcmp(l,"None") || !strcmp(l,"NONE") || !strcmp(l,"none"))
            {
                cmdexecuter->discardDeleteAll();
            }
            else
            {
                //Do nth, ask again
                OUTSTREAM << "Please enter [y]es/[n]o/[a]ll/none: " << flush;
            }
        break;
        case LOGINPASSWORD:
        {
            if (!strlen(l))
            {
                break;
            }
            if (cmdexecuter->confirming)
            {
                cmdexecuter->confirmWithPassword(l);
            }
            else if (cmdexecuter->confirmingcancel)
            {
                cmdexecuter->confirmCancel(cmdexecuter->link.c_str(), l);
            }
            else
            {
                cmdexecuter->loginWithPassword(l);
            }

            cmdexecuter->confirming = false;
            cmdexecuter->confirmingcancel = false;

            setprompt(COMMAND);
            break;
        }
        case NEWPASSWORD:
        {
            if (!strlen(l))
            {
                break;
            }
            newpasswd = l;
            OUTSTREAM << endl;
            setprompt(PASSWORDCONFIRM);
        }
        break;

        case PASSWORDCONFIRM:
        {
            if (!strlen(l))
            {
                break;
            }
            if (l != newpasswd)
            {
                OUTSTREAM << endl << "New passwords differ, please try again" << endl;
            }
            else
            {
                OUTSTREAM << endl;
                if (!cmdexecuter->signingup)
                {
                    cmdexecuter->changePassword(newpasswd.c_str());
                }
                else
                {
                    cmdexecuter->signupWithPassword(l);
                    cmdexecuter->signingup = false;
                }
            }

            setprompt(COMMAND);
            break;
        }

        case COMMAND:
        {
            if (!l || !strcmp(l, "q") || !strcmp(l, "quit") || !strcmp(l, "exit") || !strcmp(l, "exit ") || !strcmp(l, "quit "))
            {
                //                store_line(NULL);
                return true; // exit
            }

            else  if (!strncmp(l,"sendack",strlen("sendack")) ||
                      !strncmp(l,"Xsendack",strlen("Xsendack")))
            {
                string sack="ack";
                cm->informStateListeners(sack);
                break;
            }

#if defined(_WIN32) || defined(__APPLE__)
            else if (!strcmp(l, "update") || !strcmp(l, "update ")) //if extra args are received, it'll be processed by executer
            {
                string confirmationQuery("This might require restarting MEGAcmd. Are you sure to continue");
                confirmationQuery+="? (Yes/No): ";

                int confirmationResponse = askforConfirmation(confirmationQuery);

                if (confirmationResponse != MCMDCONFIRM_YES && confirmationResponse != MCMDCONFIRM_ALL)
                {
                    setCurrentOutCode(MCMD_INVALIDSTATE); // so as not to indicate already updated
                    return false;
                }
                bool restartRequired = false;

                if (!executeUpdater(&restartRequired))
                {
                    setCurrentOutCode(MCMD_INVALIDSTATE); // so as not to indicate already updated
                    return false;
                }

                if (restartRequired && restartServer())
                {
                    OUTSTREAM << " " << endl;

                    int attempts=20; //give a while for ingoin petitions to end before killing the server
                    while(petitionThreads.size() > 1 && attempts--)
                    {
                        sleepSeconds(20-attempts);
                    }
                    return true;
                }
                else
                {
                    OUTSTREAM << "Update is not required. You are in the last version. Further info: \"version --help\", \"update --help\"" << endl;
                    return false;
                }
            }
#endif
            executecommand(l);
            break;
        }
    }
    return false; //Do not exit
}

void * doProcessLine(void *pointer)
{
    CmdPetition *inf = (CmdPetition*)pointer;

    OUTSTRINGSTREAM s;

    setCurrentThreadLogLevel(MegaApi::LOG_LEVEL_ERROR);
    setCurrentOutCode(MCMD_OK);
    setCurrentPetition(inf);
    LoggedStreamPartialOutputs ls(cm, inf);
    setCurrentThreadOutStream(&ls);

    if (inf->getLine() && *(inf->getLine())=='X')
    {
        setCurrentThreadIsCmdShell(true);
        char * aux = inf->line;
        inf->line=strdup(inf->line+1);
        free(aux);
    }
    else
    {
        setCurrentThreadIsCmdShell(false);
    }

    LOG_verbose << " Processing " << inf->line << " in thread: " << MegaThread::currentThreadId() << " " << cm->get_petition_details(inf);

    doExit = process_line(inf->getLine());

    if (doExit)
    {
        stopcheckingforUpdaters = true;
        LOG_verbose << " Exit registered upon process_line: " ;
    }

    LOG_verbose << " Procesed " << inf->line << " in thread: " << MegaThread::currentThreadId() << " " << cm->get_petition_details(inf);

    MegaThread * petitionThread = inf->getPetitionThread();
    cm->returnAndClosePetition(inf, &s, getCurrentOutCode());

    semaphoreClients.release();

    if (doExit && (!interactiveThread() || getCurrentThreadIsCmdShell() ))
    {
        cm->stopWaiting();
    }

    mutexEndedPetitionThreads.lock();
    endedPetitionThreads.push_back(petitionThread);
    mutexEndedPetitionThreads.unlock();

    return NULL;
}


int askforConfirmation(string message)
{
    CmdPetition *inf = getCurrentPetition();
    if (inf)
    {
        return cm->getConfirmation(inf,message);
    }
    else
    {
        LOG_err << "Unable to get current petition to ask for confirmation";
    }

    return MCMDCONFIRM_NO;
}

string askforUserResponse(string message)
{
    CmdPetition *inf = getCurrentPetition();
    if (inf)
    {
        return cm->getUserResponse(inf,message);
    }
    else
    {
        LOG_err << "Unable to get current petition to ask for confirmation";
    }

    return string("NOCURRENPETITION");
}



void delete_finished_threads()
{
    mutexEndedPetitionThreads.lock();
    for (std::vector<MegaThread *>::iterator it = endedPetitionThreads.begin(); it != endedPetitionThreads.end(); )
    {
        MegaThread *mt = (MegaThread*)*it;
        for (std::vector<MegaThread *>::iterator it2 = petitionThreads.begin(); it2 != petitionThreads.end(); )
        {
            if (mt == (MegaThread*)*it2)
            {
                it2 = petitionThreads.erase(it2);
            }
            else
            {
                ++it2;
            }
        }

        mt->join();
        delete mt;
        it = endedPetitionThreads.erase(it);
    }
    mutexEndedPetitionThreads.unlock();
}



void finalize()
{
    static bool alreadyfinalized = false;
    if (alreadyfinalized)
        return;
    alreadyfinalized = true;
    LOG_info << "closing application ...";
    delete_finished_threads();
    delete cm;
    if (!consoleFailed)
    {
        delete console;
    }

    delete megaCmdMegaListener;
    if (threadRetryConnections)
    {
        threadRetryConnections->join();
    }
    delete threadRetryConnections;
    delete api;

    while (!apiFolders.empty())
    {
        delete apiFolders.front();
        apiFolders.pop();
    }

    for (std::vector< MegaApi * >::iterator it = occupiedapiFolders.begin(); it != occupiedapiFolders.end(); ++it)
    {
        delete ( *it );
    }

    occupiedapiFolders.clear();

    delete megaCmdGlobalListener;
    delete cmdexecuter;

    LOG_debug << "resources have been cleaned ...";
    delete loggerCMD;
    ConfigurationManager::unlockExecution();
    ConfigurationManager::unloadConfiguration();

}

int currentclientID = 1;

void * retryConnections(void *pointer)
{
    while(!doExit)
    {
        LOG_verbose << "Calling recurrent retryPendingConnections";
        api->retryPendingConnections();

        int count = 100;
        while (!doExit && --count)
        {
            sleepMilliSeconds(300);
        }
    }
    return NULL;
}


void startcheckingForUpdates()
{
    ConfigurationManager::savePropertyValue("autoupdate", 1);

    if (!alreadyCheckingForUpdates)
    {
        alreadyCheckingForUpdates = true;
        LOG_info << "Starting autoupdate check mechanism";
        MegaThread *checkupdatesThread = new MegaThread();
        checkupdatesThread->start(checkForUpdates,checkupdatesThread);
    }
}

void stopcheckingForUpdates()
{
    ConfigurationManager::savePropertyValue("autoupdate", 0);

    stopcheckingforUpdaters = true;
}

void* checkForUpdates(void *param)
{
    stopcheckingforUpdaters = false;
    LOG_debug << "Initiating recurrent checkForUpdates";

    int secstosleep = 60;
    while (secstosleep>0 && !stopcheckingforUpdaters)
    {
        sleepSeconds(2);
        secstosleep-=2;
    }

    while (!doExit && !stopcheckingforUpdaters)
    {
        bool restartRequired = false;
        if (!executeUpdater(&restartRequired, true)) //only download & check
        {
            LOG_err << " Failed to execute updater";
        }
        else if (restartRequired)
        {
            LOG_info << " There is a pending update. Will be applied in a few seconds";

            broadcastMessage("A new update has been downloaded. It will be performed in 60 seconds");
            int secstosleep = 57;
            while (secstosleep>0 && !stopcheckingforUpdaters)
            {
                sleepSeconds(2);
                secstosleep-=2;
            }
            if (stopcheckingforUpdaters) break;
            broadcastMessage("  Executing update in 3");
            sleepSeconds(1);
            if (stopcheckingforUpdaters) break;
            broadcastMessage("  Executing update in 2");
            sleepSeconds(1);
            if (stopcheckingforUpdaters) break;
            broadcastMessage("  Executing update in 1");
            sleepSeconds(1);
            if (stopcheckingforUpdaters) break;

            while(petitionThreads.size() && !stopcheckingforUpdaters)
            {
                LOG_fatal << " waiting for petitions to end to initiate upload " << petitionThreads.size() << petitionThreads.at(0);
                sleepSeconds(2);
                delete_finished_threads();
            }

            if (stopcheckingforUpdaters) break;
            broadcastMessage("  Executing update    !");
            LOG_info << " Applying update";
            executeUpdater(&restartRequired);
        }
        else
        {
            LOG_verbose << " There is no pending update";
        }

        if (stopcheckingforUpdaters) break;
        if (restartRequired && restartServer())
        {
            int attempts=20; //give a while for ingoin petitions to end before killing the server
            while(petitionThreads.size() && attempts--)
            {
                sleepSeconds(20-attempts);
                delete_finished_threads();
            }

            doExit = true;
            cm->stopWaiting();
            break;
        }

        int secstosleep = 7200;
        while (secstosleep>0 && !stopcheckingforUpdaters)
        {
            sleepSeconds(2);
            secstosleep-=2;
        }
    }

    alreadyCheckingForUpdates = false;

    delete (MegaThread *)param;
    return NULL;
}

// main loop
void megacmd()
{
    threadRetryConnections = new MegaThread();
    threadRetryConnections->start(retryConnections, NULL);

    LOG_info << "Listening to petitions ... ";

    for (;; )
    {
        cm->waitForPetition();

        api->retryPendingConnections();

        if (doExit)
        {
            LOG_verbose << "closing after wait ..." ;
            return;
        }

        if (cm->receivedPetition())
        {

            LOG_verbose << "Client connected ";

            CmdPetition *inf = cm->getPetition();

            LOG_verbose << "petition registered: " << inf->line;

            delete_finished_threads();

            if (!inf || !strcmp(inf->getLine(),"ERROR"))
            {
                LOG_warn << "Petition couldn't be registered. Dismissing it.";
                delete inf;
            }
            // if state register petition
            else  if (!strncmp(inf->getLine(),"registerstatelistener",strlen("registerstatelistener")) ||
                      !strncmp(inf->getLine(),"Xregisterstatelistener",strlen("Xregisterstatelistener")))
            {

                cm->registerStateListener(inf);

                // communicate client ID
                string s = "clientID:";
                s+=SSTR(currentclientID);
                s+=(char)0x1F;
                inf->clientID = currentclientID;
                currentclientID++;

                cm->informStateListener(inf,s);

#if defined(_WIN32) || defined(__APPLE__)
                string message="";
                ostringstream os;
                MegaCmdListener *megaCmdListener = new MegaCmdListener(NULL);
                api->getLastAvailableVersion("BdARkQSQ",megaCmdListener);
                if (!megaCmdListener->trywait(2000))
                {
                    if (!megaCmdListener->getError())
                    {
                        LOG_fatal << "No MegaError at getLastAvailableVersion: ";
                    }
                    else if (megaCmdListener->getError()->getErrorCode() != MegaError::API_OK)
                    {
                        LOG_debug << "Couldn't get latests available version: " << megaCmdListener->getError()->getErrorString();
                    }
                    else
                    {
                        if (megaCmdListener->getRequest()->getNumber() != MEGACMD_CODE_VERSION)
                        {
                            os << "---------------------------------------------------------------------" << endl;
                            os << "--        There is a new version available of megacmd: " << setw(12) << left << megaCmdListener->getRequest()->getName() << "--" << endl;
                            os << "--        Please, update this one: See \"update --help\".          --" << endl;
                            os << "--        Or download the latest from https://mega.nz/cmd          --" << endl;
#if defined(__APPLE__)
                            os << "--        Before installing enter \"exit\" to close MEGAcmd          --" << endl;
#endif
                            os << "---------------------------------------------------------------------" << endl;
                        }
                    }
                    delete megaCmdListener;
                }
                else
                {
                    LOG_debug << "Couldn't get latests available version (petition timed out)";

                    api->removeRequestListener(megaCmdListener);
                    delete megaCmdListener;
                }

                int autoupdate = ConfigurationManager::getConfigurationValue("autoupdate", -1);
                if (autoupdate == -1 || autoupdate == 2)
                {
                    os << "ENABLING AUTOUPDATE BY DEFAULT. You can disable it with \"update --auto=off\"" << endl;
                    autoupdate = 1;
                }

                if (autoupdate == 1)
                {
                    startcheckingForUpdates();
                }
                message=os.str();


                if (message.size())
                {
                    s += "message:";
                    s+=message;
                    s+=(char)0x1F;
                }
#endif

                bool isOSdeprecated = false;
#ifdef MEGACMD_DEPRECATED_OS
                isOSdeprecated = true;
#endif

#ifdef __APPLE__
                char releaseStr[256];
                size_t size = sizeof(releaseStr);
                if (!sysctlbyname("kern.osrelease", releaseStr, &size, NULL, 0)  && size > 0)
                {
                    if (strchr(releaseStr,'.'))
                    {
                        char *token = strtok(releaseStr, ".");
                        if (token)
                        {
                            errno = 0;
                            char *endPtr = NULL;
                            long majorVersion = strtol(token, &endPtr, 10);
                            if (endPtr != token && errno != ERANGE && majorVersion >= INT_MIN && majorVersion <= INT_MAX)
                            {
                                if((int)majorVersion < 13) // Older versions from 10.9 (mavericks)
                                {
                                    isOSdeprecated = true;
                                }
                            }
                        }
                    }
                }
#endif
                if (isOSdeprecated)
                {
                    s += "message:";
                    s += "Your Operative System is too old.\n";
                    s += "You might not receive new updates for this application.\n";
                    s += "We strongly recommend you to update to a new version.\n";
                    s+=(char)0x1F;
                }

                if (sandboxCMD->storageStatus != MegaApi::STORAGE_STATE_GREEN)
                {
                    s += "message:";

                    if (sandboxCMD->storageStatus == MegaApi::STORAGE_STATE_RED)
                    {
                        s+= "You have exeeded your available storage.\n";
                    }
                    else
                    {
                        s+= "You are running out of available storage.\n";
                    }
                    s+="You can change your account plan to increase your quota limit.\nSee \"help --upgrade\" for further details";
                    s+=(char)0x1F;
                }

                // communicate status info
                s+= "prompt:";
                s+=dynamicprompt;
                s+=(char)0x1F;

                cmdexecuter->checkAndInformPSA(inf);

                cm->informStateListener(inf,s);
            }
            else
            { // normal petition

                semaphoreClients.wait();

                //append new one
                MegaThread * petitionThread = new MegaThread();

                petitionThreads.push_back(petitionThread);
                inf->setPetitionThread(petitionThread);

                LOG_verbose << "starting processing: <" << inf->line << ">";

                petitionThread->start(doProcessLine, (void*)inf);
            }
        }
    }
}

class NullBuffer : public std::streambuf
{
public:
    int overflow(int c)
    {
        return c;
    }
};

void printWelcomeMsg()
{
    unsigned int width = getNumberOfCols(75);

#ifdef _WIN32
        width--;
#endif

    COUT << endl;
    COUT << ".";
    for (unsigned int i = 0; i < width; i++)
        COUT << "=" ;
    COUT << ".";
    COUT << endl;
    printCenteredLine(" __  __ _____ ____    _                      _ ",width);
    printCenteredLine("|  \\/  | ___|/ ___|  / \\   ___ _ __ ___   __| |",width);
    printCenteredLine("| |\\/| | \\  / |  _  / _ \\ / __| '_ ` _ \\ / _` |",width);
    printCenteredLine("| |  | | /__\\ |_| |/ ___ \\ (__| | | | | | (_| |",width);
    printCenteredLine("|_|  |_|____|\\____/_/   \\_\\___|_| |_| |_|\\__,_|",width);

    COUT << "|";
    for (unsigned int i = 0; i < width; i++)
        COUT << " " ;
    COUT << "|";
    COUT << endl;
    printCenteredLine("SERVER",width);

    COUT << "`";
    for (unsigned int i = 0; i < width; i++)
        COUT << "=" ;
    COUT << "´";
    COUT << endl;

}

#ifdef __MACH__


bool enableSetuidBit()
{
    char *response = runWithRootPrivileges("do shell script \"chown root /Applications/MEGAcmd.app/Contents/MacOS/MEGAcmdLoader && chmod 4755 /Applications/MEGAcmd.app/Contents/MacOS/MEGAcmdLoader && echo true\"");
    if (!response)
    {
        return NULL;
    }
    bool result = strlen(response) >= 4 && !strncmp(response, "true", 4);
    delete response;
    return result;
}


void initializeMacOSStuff(int argc, char* argv[])
{
#ifndef NDEBUG
        return;
#endif

    int fd = -1;
    if (argc)
    {
        long int value = strtol(argv[argc-1], NULL, 10);
        if (value > 0 && value < INT_MAX)
        {
            fd = value;
        }
    }

    if (fd < 0)
    {
        if (!enableSetuidBit())
        {
            ::exit(0);
        }

        //Reboot
        if (fork() )
        {
            execv("/Applications/MEGAcmd.app/Contents/MacOS/MEGAcmdLoader",argv);
        }
        sleep(10);
        ::exit(0);
    }
}

#endif

string getLocaleCode()
{
#if defined(_WIN32) && defined(LOCALE_SISO639LANGNAME)
    LCID lcidLocaleId;
    LCTYPE lctyLocaleInfo;
    PWSTR pstr;
    INT iBuffSize;

    lcidLocaleId = LOCALE_USER_DEFAULT;
    lctyLocaleInfo = LOCALE_SISO639LANGNAME;

    // Determine the size
    iBuffSize = GetLocaleInfo( lcidLocaleId, lctyLocaleInfo, NULL, 0 );

    if(iBuffSize > 0)
    {
        pstr = (WCHAR *) malloc( iBuffSize * sizeof(WCHAR) );
        if(pstr != NULL)
        {
            if(GetLocaleInfoW( lcidLocaleId, lctyLocaleInfo, pstr, iBuffSize ))
            {
                string toret;
                std::wstring ws(pstr);
                localwtostring(&ws,&toret);
                free(pstr); //free locale info string
                return toret;
            }
            free(pstr); //free locale info string
        }
    }

#else

    try
     {
        locale l("");

        string ls = l.name();
        size_t posequal = ls.find("=");
        size_t possemicolon = ls.find_first_of(";.");

        if (posequal != string::npos && possemicolon != string::npos && posequal < possemicolon)
        {
            return ls.substr(posequal+1,possemicolon-posequal-1);
        }
     }
     catch (const std::exception& e)
     {
#ifndef __MACH__
        std::cerr << "Warning: unable to get locale " << std::endl;
#endif
     }

#endif
    return string();

}

bool runningInBackground()
{
#ifndef _WIN32
    pid_t fg = tcgetpgrp(STDIN_FILENO);
    if(fg == -1) {
        // Piped:
        return false;
    }  else if (fg == getpgrp()) {
        // foreground
        return false;
    } else {
        // background
        return true;
    }
#endif
    return false;
}

#ifndef MEGACMD_USERAGENT_SUFFIX
#define MEGACMD_USERAGENT_SUFFIX
#define MEGACMD_STRINGIZE(x)
#else
#define MEGACMD_STRINGIZE2(x) "-" #x
#define MEGACMD_STRINGIZE(x) MEGACMD_STRINGIZE2(x)
#endif

bool extractarg(vector<const char*>& args, const char *what)
{
    for (int i = int(args.size()); i--; )
    {
        if (!strcmp(args[i], what))
        {
            args.erase(args.begin() + i);
            return true;
        }
    }
    return false;
}

bool extractargparam(vector<const char*>& args, const char *what, std::string& param)
{
    for (int i = int(args.size()) - 1; --i >= 0; )
    {
        if (!strcmp(args[i], what) && args.size() > i)
        {
            param = args[i + 1];
            args.erase(args.begin() + i, args.begin() + i + 2);
            return true;
        }
    }
    return false;
}


#ifndef _WIN32
#include <sys/wait.h>
bool is_pid_running(pid_t pid) {

    while(waitpid(-1, 0, WNOHANG) > 0) {
        // Wait for defunct....
    }

    if (0 == kill(pid, 0))
        return 1; // Process exists

    return 0;
}
#endif

#ifdef _WIN32
LPTSTR getCurrentSid()
{
    HANDLE hTok = NULL;
    LPBYTE buf = NULL;
    DWORD  dwSize = 0;
    LPTSTR stringSID = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hTok))
    {
        GetTokenInformation(hTok, TokenUser, NULL, 0, &dwSize);
        if (dwSize)
        {
            buf = (LPBYTE)LocalAlloc(LPTR, dwSize);
            if (GetTokenInformation(hTok, TokenUser, buf, dwSize, &dwSize))
            {
                ConvertSidToStringSid(((PTOKEN_USER)buf)->User.Sid, &stringSID);
            }
            LocalFree(buf);
        }
        CloseHandle(hTok);
    }
    return stringSID;
}
#endif

bool registerUpdater()
{
#ifdef _WIN32
    ITaskService *pService = NULL;
    ITaskFolder *pRootFolder = NULL;
    ITaskFolder *pMEGAFolder = NULL;
    ITaskDefinition *pTask = NULL;
    IRegistrationInfo *pRegInfo = NULL;
    IPrincipal *pPrincipal = NULL;
    ITaskSettings *pSettings = NULL;
    IIdleSettings *pIdleSettings = NULL;
    ITriggerCollection *pTriggerCollection = NULL;
    ITrigger *pTrigger = NULL;
    IDailyTrigger *pCalendarTrigger = NULL;
    IRepetitionPattern *pRepetitionPattern = NULL;
    IActionCollection *pActionCollection = NULL;
    IAction *pAction = NULL;
    IExecAction *pExecAction = NULL;
    IRegisteredTask *pRegisteredTask = NULL;
    time_t currentTime;
    struct tm* currentTimeInfo;
    WCHAR currentTimeString[128];
    _bstr_t taskBaseName = L"MEGAcmd Update Task ";
    LPTSTR stringSID = NULL;
    bool success = false;

    stringSID = getCurrentSid();
    if (!stringSID)
    {
        MegaApi::log(MegaApi::LOG_LEVEL_ERROR, "Unable to get the current SID");
        return false;
    }

    time(&currentTime);
    currentTimeInfo = localtime(&currentTime);
    wcsftime(currentTimeString, 128,  L"%Y-%m-%dT%H:%M:%S", currentTimeInfo);
    _bstr_t taskName = taskBaseName + stringSID;
    _bstr_t userId = stringSID;
    LocalFree(stringSID);

    TCHAR MEGAcmdUpdaterPath[MAX_PATH];

    if (!SUCCEEDED(GetModuleFileName(NULL, MEGAcmdUpdaterPath , MAX_PATH)))
    {
        LOG_err << "Couldnt get EXECUTABLE folder: " << wstring(MEGAcmdUpdaterPath);
        return false;
    }

    if (SUCCEEDED(PathRemoveFileSpec(MEGAcmdUpdaterPath)))
    {
        if (!PathAppend(MEGAcmdUpdaterPath,TEXT("MEGAcmdUpdater.exe")))
        {
            LOG_err << "Couldnt append MEGAcmdUpdater exec: " << wstring(MEGAcmdUpdaterPath);
            return false;
        }
    }
    else
    {
        LOG_err << "Couldnt remove file spec: " << wstring(MEGAcmdUpdaterPath);
        return false;
    }

    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    if (SUCCEEDED(CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, 0, NULL))
            && SUCCEEDED(CoCreateInstance(CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER, IID_ITaskService, (void**)&pService))
            && SUCCEEDED(pService->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t()))
            && SUCCEEDED(pService->GetFolder(_bstr_t( L"\\"), &pRootFolder)))
    {
        if (pRootFolder->CreateFolder(_bstr_t(L"MEGA"), _variant_t(L""), &pMEGAFolder) == 0x800700b7)
        {
            pRootFolder->GetFolder(_bstr_t(L"MEGA"), &pMEGAFolder);
        }

        if (pMEGAFolder
                && SUCCEEDED(pService->NewTask(0, &pTask))
                && SUCCEEDED(pTask->get_RegistrationInfo(&pRegInfo))
                && SUCCEEDED(pRegInfo->put_Author(_bstr_t(L"MEGA Limited")))
                && SUCCEEDED(pTask->get_Principal(&pPrincipal))
                && SUCCEEDED(pPrincipal->put_Id(_bstr_t(L"Principal1")))
                && SUCCEEDED(pPrincipal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN))
                && SUCCEEDED(pPrincipal->put_RunLevel(TASK_RUNLEVEL_LUA))
                && SUCCEEDED(pPrincipal->put_UserId(userId))
                && SUCCEEDED(pTask->get_Settings(&pSettings))
                && SUCCEEDED(pSettings->put_StartWhenAvailable(VARIANT_TRUE))
                && SUCCEEDED(pSettings->put_DisallowStartIfOnBatteries(VARIANT_FALSE))
                && SUCCEEDED(pSettings->get_IdleSettings(&pIdleSettings))
                && SUCCEEDED(pIdleSettings->put_StopOnIdleEnd(VARIANT_FALSE))
                && SUCCEEDED(pIdleSettings->put_RestartOnIdle(VARIANT_FALSE))
                && SUCCEEDED(pIdleSettings->put_WaitTimeout(_bstr_t()))
                && SUCCEEDED(pIdleSettings->put_IdleDuration(_bstr_t()))
                && SUCCEEDED(pTask->get_Triggers(&pTriggerCollection))
                && SUCCEEDED(pTriggerCollection->Create(TASK_TRIGGER_DAILY, &pTrigger))
                && SUCCEEDED(pTrigger->QueryInterface(IID_IDailyTrigger, (void**) &pCalendarTrigger))
                && SUCCEEDED(pCalendarTrigger->put_Id(_bstr_t(L"Trigger1")))
                && SUCCEEDED(pCalendarTrigger->put_DaysInterval(1))
                && SUCCEEDED(pCalendarTrigger->put_StartBoundary(_bstr_t(currentTimeString)))
                && SUCCEEDED(pCalendarTrigger->get_Repetition(&pRepetitionPattern))
                && SUCCEEDED(pRepetitionPattern->put_Duration(_bstr_t(L"P1D")))
                && SUCCEEDED(pRepetitionPattern->put_Interval(_bstr_t(L"PT2H")))
                && SUCCEEDED(pRepetitionPattern->put_StopAtDurationEnd(VARIANT_FALSE))
                && SUCCEEDED(pTask->get_Actions(&pActionCollection))
                && SUCCEEDED(pActionCollection->Create(TASK_ACTION_EXEC, &pAction))
                && SUCCEEDED(pAction->QueryInterface(IID_IExecAction, (void**)&pExecAction))
                && SUCCEEDED(pExecAction->put_Path(_bstr_t(MEGAcmdUpdaterPath)))
                && SUCCEEDED(pExecAction->put_Arguments(_bstr_t(L"--emergency-update"))))
        {
            if (SUCCEEDED(pMEGAFolder->RegisterTaskDefinition(taskName, pTask,
                    TASK_CREATE_OR_UPDATE, _variant_t(), _variant_t(),
                    TASK_LOGON_INTERACTIVE_TOKEN, _variant_t(L""),
                    &pRegisteredTask)))
            {
                success = true;
                MegaApi::log(MegaApi::LOG_LEVEL_ERROR, "Update task registered OK");
            }
            else
            {
                MegaApi::log(MegaApi::LOG_LEVEL_ERROR, "Error registering update task");
            }
        }
        else
        {
            MegaApi::log(MegaApi::LOG_LEVEL_ERROR, "Error creating update task");
        }
    }
    else
    {
        MegaApi::log(MegaApi::LOG_LEVEL_ERROR, "Error getting root task folder");
    }

    if (pRegisteredTask)
    {
        pRegisteredTask->Release();
    }
    if (pTrigger)
    {
        pTrigger->Release();
    }
    if (pTriggerCollection)
    {
        pTriggerCollection->Release();
    }
    if (pIdleSettings)
    {
        pIdleSettings->Release();
    }
    if (pSettings)
    {
        pSettings->Release();
    }
    if (pPrincipal)
    {
        pPrincipal->Release();
    }
    if (pRegInfo)
    {
        pRegInfo->Release();
    }
    if (pCalendarTrigger)
    {
        pCalendarTrigger->Release();
    }
    if (pAction)
    {
        pAction->Release();
    }
    if (pActionCollection)
    {
        pActionCollection->Release();
    }
    if (pRepetitionPattern)
    {
        pRepetitionPattern->Release();
    }
    if (pExecAction)
    {
        pExecAction->Release();
    }
    if (pTask)
    {
        pTask->Release();
    }
    if (pMEGAFolder)
    {
        pMEGAFolder->Release();
    }
    if (pRootFolder)
    {
        pRootFolder->Release();
    }
    if (pService)
    {
        pService->Release();
    }

    return success;
#elif defined(__MACH__)
    return registerUpdateDaemon();
#else
    return true;
#endif
}

#ifdef _WIN32
void uninstall()
{
    ITaskService *pService = NULL;
    ITaskFolder *pRootFolder = NULL;
    ITaskFolder *pMEGAFolder = NULL;
    _bstr_t taskBaseName = L"MEGAcmd Update Task ";
    LPTSTR stringSID = NULL;

    stringSID = getCurrentSid();
    if (!stringSID)
    {
        MegaApi::log(MegaApi::LOG_LEVEL_ERROR, "Unable to get the current SID");
        return;
    }
    _bstr_t taskName = taskBaseName + stringSID;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    if (SUCCEEDED(CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, 0, NULL))
            && SUCCEEDED(CoCreateInstance(CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER, IID_ITaskService, (void**)&pService))
            && SUCCEEDED(pService->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t()))
            && SUCCEEDED(pService->GetFolder(_bstr_t( L"\\"), &pRootFolder)))
    {
        if (pRootFolder->CreateFolder(_bstr_t(L"MEGA"), _variant_t(L""), &pMEGAFolder) == 0x800700b7)
        {
            pRootFolder->GetFolder(_bstr_t(L"MEGA"), &pMEGAFolder);
        }

        if (pMEGAFolder)
        {
            pMEGAFolder->DeleteTask(taskName, 0);
            pMEGAFolder->Release();
        }
        pRootFolder->Release();
    }

    if (pService)
    {
        pService->Release();
    }
}

#endif

} //end namespace

using namespace megacmd;

int main(int argc, char* argv[])
{
    string localecode = getLocaleCode();
#ifdef _WIN32
    // Set Environment's default locale
    setlocale(LC_ALL, "en-US");
#endif
    mcmdMainArgv = argv;
    mcmdMainArgc = argc;

#ifdef __MACH__
    initializeMacOSStuff(argc,argv);
#endif

    NullBuffer null_buffer;
    std::ostream null_stream(&null_buffer);
#ifndef ENABLE_LOG_PERFORMANCE
    SimpleLogger::setAllOutputs(&null_stream);
#endif
    SimpleLogger::setLogLevel(logMax); // do not filter anything here, log level checking is done by loggerCMD

    loggerCMD = new MegaCMDLogger();

    loggerCMD->setApiLoggerLevel(MegaApi::LOG_LEVEL_ERROR);
    loggerCMD->setCmdLoggerLevel(MegaApi::LOG_LEVEL_INFO);

    string loglevelenv;
#ifndef _WIN32
    loglevelenv = (getenv ("MEGACMD_LOGLEVEL") == NULL)?"":getenv ("MEGACMD_LOGLEVEL");
#endif

    vector<const char*> args;
    if (argc > 1)
    {
        args = vector<const char*>(argv + 1, argv + argc);
    }

    string debug_api_url;
    bool debug = extractarg(args, "--debug");
    bool debugfull = extractarg(args, "--debug-full");
    bool verbose = extractarg(args, "--verbose");
    bool verbosefull = extractarg(args, "--verbose-full");
    bool skiplockcheck = extractarg(args, "--skip-lock-check");
    bool setapiurl = extractargparam(args, "--apiurl", debug_api_url);  // only for debugging
    bool disablepkp = extractarg(args, "--disablepkp");  // only for debugging


#ifdef _WIN32
    bool buninstall = extractarg(args, "--uninstall") || extractarg(args, "/uninstall");
    if (buninstall)
    {
        MegaApi::removeRecursively(ConfigurationManager::getConfigFolder().c_str());
        uninstall();
        exit(0);
    }
#endif

    string shandletowait;
    bool dowaitforhandle = extractargparam(args, "--wait-for", shandletowait);
    if (dowaitforhandle)
    {
#ifdef _WIN32
        DWORD processId = atoi(shandletowait.c_str());

        HANDLE processHandle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, processId);

        cout << "Waiting for former server to end" << endl;
        WaitForSingleObject( processHandle, INFINITE );
        CloseHandle(processHandle);
#else

        pid_t processId = atoi(shandletowait.c_str());

        cout << "Waiting for former server to end... " << endl;
        while (is_pid_running(processId))
        {
            sleep(1);
        }
#endif
    }

    if (!loglevelenv.compare("DEBUG") || debug )
    {
        loggerCMD->setCmdLoggerLevel(MegaApi::LOG_LEVEL_DEBUG);
    }
    if (!loglevelenv.compare("FULLDEBUG") || debugfull )
    {
        loggerCMD->setApiLoggerLevel(MegaApi::LOG_LEVEL_DEBUG);
        loggerCMD->setCmdLoggerLevel(MegaApi::LOG_LEVEL_DEBUG);
    }
    if (!loglevelenv.compare("VERBOSE") || verbose )
    {
        loggerCMD->setCmdLoggerLevel(MegaApi::LOG_LEVEL_MAX);
    }
    if (!loglevelenv.compare("FULLVERBOSE") || verbosefull )
    {
        loggerCMD->setApiLoggerLevel(MegaApi::LOG_LEVEL_MAX);
        loggerCMD->setCmdLoggerLevel(MegaApi::LOG_LEVEL_MAX);
    }

    ConfigurationManager::loadConfiguration(( argc > 1 ) && debug);
    if (!ConfigurationManager::lockExecution() && !skiplockcheck)
    {
        cerr << "Another instance of MEGAcmd Server is running. Execute with --skip-lock-check to force running (NOT RECOMMENDED)" << endl;
        sleepSeconds(5);
        exit(-2);
    }

    char userAgent[40];
    sprintf(userAgent, "MEGAcmd" MEGACMD_STRINGIZE(MEGACMD_USERAGENT_SUFFIX) "/%d.%d.%d.0", MEGACMD_MAJOR_VERSION,MEGACMD_MINOR_VERSION,MEGACMD_MICRO_VERSION);

    MegaApi::addLoggerObject(loggerCMD);
    MegaApi::setLogLevel(MegaApi::LOG_LEVEL_MAX);

    LOG_debug << "MEGAcmd version: " << MEGACMD_MAJOR_VERSION << "." << MEGACMD_MINOR_VERSION << "." << MEGACMD_MICRO_VERSION << ": code " << MEGACMD_CODE_VERSION;

#if defined(__MACH__) && defined(ENABLE_SYNC)
    int fd = -1;
    if (argc)
    {
        long int value = strtol(argv[argc-1], NULL, 10);
        if (value > 0 && value < INT_MAX)
        {
            fd = value;
        }
    }

    if (fd >= 0)
    {
        api = new MegaApi("BdARkQSQ", ConfigurationManager::getConfigFolder().c_str(), userAgent, fd);
    }
    else
    {
        api = new MegaApi("BdARkQSQ", (MegaGfxProcessor*)NULL, ConfigurationManager::getConfigFolder().c_str(), userAgent);
    }
#else
    api = new MegaApi("BdARkQSQ", (MegaGfxProcessor*)NULL, ConfigurationManager::getConfigFolder().c_str(), userAgent);
#endif

    if (setapiurl)
    {
        api->changeApiUrl(debug_api_url.c_str(), disablepkp);
    }


    api->setLanguage(localecode.c_str());

    for (int i = 0; i < 5; i++)
    {
        MegaApi *apiFolder = new MegaApi("BdARkQSQ", (MegaGfxProcessor*)NULL, (const char*)NULL, userAgent);
        apiFolder->setLanguage(localecode.c_str());
        apiFolders.push(apiFolder);
        apiFolder->setLogLevel(MegaApi::LOG_LEVEL_MAX);
        semaphoreapiFolders.release();
    }

    for (int i = 0; i < 100; i++)
    {
        semaphoreClients.release();
    }

    LOG_debug << "Language set to: " << localecode;

    sandboxCMD = new MegaCmdSandbox();
    cmdexecuter = new MegaCmdExecuter(api, loggerCMD, sandboxCMD);

    megaCmdGlobalListener = new MegaCmdGlobalListener(loggerCMD, sandboxCMD);
    megaCmdMegaListener = new MegaCmdMegaListener(api, NULL, sandboxCMD);
    api->addGlobalListener(megaCmdGlobalListener);
    api->addListener(megaCmdMegaListener);

    // set up the console
#ifdef _WIN32
    console = new CONSOLE_CLASS;
#else
    struct termios term;
    if ( ( tcgetattr(STDIN_FILENO, &term) < 0 ) || runningInBackground() ) //try console
    {
        consoleFailed = true;
        console = NULL;
    }
    else
    {
        console = new CONSOLE_CLASS;
    }
#endif
    cm = new COMUNICATIONMANAGER();

#if _WIN32
    if( SetConsoleCtrlHandler( (PHANDLER_ROUTINE) CtrlHandler, TRUE ) )
     {
        LOG_debug << "Control handler set";
     }
     else
     {
        LOG_warn << "Control handler set";
     }
#else
    // prevent CTRL+C exit
    if (!consoleFailed){
        signal(SIGINT, sigint_handler);
    }
#endif

    atexit(finalize);


#if defined(_WIN32) || defined(__APPLE__)
    if (!ConfigurationManager::getConfigurationValue("updaterregistered", false))
    {
        LOG_debug << "Registering automatic updater";
        if (registerUpdater())
        {
            ConfigurationManager::savePropertyValue("updaterregistered", true);
            LOG_verbose << "Registered automatic updater";
        }
        else
        {
            LOG_err << "Failed to register automatic updater";
        }
    }
#endif

    printWelcomeMsg();

    if (!ConfigurationManager::session.empty())
    {
        loginInAtStartup = true;
        stringstream logLine;
        logLine << "login " << ConfigurationManager::session;
        LOG_debug << "Executing ... " << logLine.str().substr(0,9) << "...";
        process_line((char*)logLine.str().c_str());
        loginInAtStartup = false;
    }

    megacmd::megacmd();
    finalize();
}

