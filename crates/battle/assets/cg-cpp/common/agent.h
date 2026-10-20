// A bot process driven like the CodinGame engine's runner drives its agents (com.codingame.gameengine.runner
// Agent / CommandLinePlayerAgent / AsynchronousWriter), for C++ ports of CodinGame referees.
//
//   cg::Agent a;
//   a.start("path/to/bot.exe", options);    // false: could not be launched
//   a.sendInput("1 2\n3\n");                // queued, written by another thread (never blocks)
//   std::string out;
//   bool ok = a.getOutput(2, 50, false, out); // 2 lines within 50 ms
//   a.readError();                          // drains the bot's stderr like the runner does
//   a.destroy();
//
// Windows and POSIX (Linux, macOS).
#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wordexp.h>
#endif

namespace cg {

// What happens to what a bot writes to its stderr
enum class Stderr {
    Discard, // thrown away
    // Like the jar: after each execution of the bot the runner reads at most 4096 bytes of its stderr
    // (Agent#readError), from a pipe of 4096 + 24 bytes on Windows (java.lang.ProcessImpl), through a
    // BufferedInputStream of 8192 bytes on POSIX. A bot writing more than that per turn blocks on its
    // stderr and times out, as it does with the jar.
    Jar,
    Inherit, // the referee's own stderr (debugging)
    File,    // written to AgentOptions::stderrFile (debugging)
};

struct AgentOptions {
    Stderr stderrMode = Stderr::Discard;
    std::string stderrFile; // with Stderr::File
    // The jar launches the bots, then spends 0.2-1 s initialising its referee before it sends them
    // their first input, so a slow bot startup hardly counts in its first turn. With a head start,
    // the first turn of a bot ends at the latest of `timeout` after its first input and
    // `headStart + timeout` after its launch (0: the time limit counts from the first input only).
    int firstTurnHeadStartMs = 0;
};

// AGENT_MAX_BUFFER_SIZE: the most bytes the runner reads from a bot in one turn
const int AGENT_MAX_BUFFER_SIZE = 10000;

class Agent {
  public:
    std::chrono::steady_clock::time_point startTime; // when the process was created

    // Launches a command line. A command with shell metacharacters (&|<>^()) runs through the shell
    // (cmd /c, sh -c), '/' becomes '\' on Windows, and on POSIX the words are split like a shell
    // does (wordexp). False when the process could not be created.
    bool start(const std::string& cmd, const AgentOptions& options) {
        opt = options;
        bool ok = spawn(cmd);
        if (!ok) return false;
        startTime = std::chrono::steady_clock::now();
        startWriter();
        return true;
    }

    // GameRunner queues each input for an AsynchronousWriter thread, so the referee never blocks on a
    // bot that does not read: a write error (the bot exited) only stops the writing.
    void sendInput(const std::string& bytes) {
        if (!writer || bytes.empty()) return;
        {
            std::lock_guard<std::mutex> g(writer->m);
            writer->q.push_back(bytes);
        }
        writer->cv.notify_one();
    }

    // Agent#getOutput + GameRunner#checkOutput: reads until `nbLine` line ends ('\n', '\r'; "\r\n"
    // counts once, even split across turns) or AGENT_MAX_BUFFER_SIZE bytes, within `timeoutMs`
    // (from now; see AgentOptions::firstTurnHeadStartMs for `firstTurn`). Bytes after the last line
    // stay buffered for the next turn. Returns false when the runner reports a timeout: not all lines
    // in time, the bot's stdout closed, or too long.
    bool getOutput(int nbLine, long long timeoutMs, bool firstTurn, std::string& out) {
        out.clear();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        if (firstTurn && opt.firstTurnHeadStartMs > 0)
            deadline = std::max(deadline, startTime + std::chrono::milliseconds(opt.firstTurnHeadStartMs + timeoutMs));
        int nbOccurences = 0;
        while ((int)out.size() < AGENT_MAX_BUFFER_SIZE && nbOccurences < nbLine) {
            if (readPos >= readBuf.size()) {
                readBuf.clear();
                readPos = 0;
                if (!fill(deadline)) break;
                continue;
            }
            char curByte = readBuf[readPos++];
            if (!(curByte == '\n' && lastCR)) {
                out += curByte;
                if (curByte == '\n' || curByte == '\r') ++nbOccurences;
            }
            lastCR = curByte == '\r';
        }
        return nbOccurences == nbLine;
    }

    // For referees that do not read their bots like the engine's runner: appends to `out` the bytes
    // received and not consumed yet, waiting until `deadline` for some when there are none. False
    // on timeout, or when the bot's stdout is closed (see closed()).
    bool read(std::string& out, std::chrono::steady_clock::time_point deadline) {
        if (readPos >= readBuf.size()) {
            readBuf.clear();
            readPos = 0;
            if (!fill(deadline)) return false;
        }
        out.append(readBuf, readPos, std::string::npos);
        readBuf.clear();
        readPos = 0;
        return true;
    }
    bool closed() const { return !outAlive; }

    // Agent#readError (with Stderr::Jar; nothing to do otherwise)
    void readError() {
        if (opt.stderrMode != Stderr::Jar) return;
#ifdef _WIN32
        // a FileInputStream: one read of at most 4096 bytes when some are available
        DWORD avail = 0;
        if (!hStderrR || !PeekNamedPipe(hStderrR, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) return;
        char tmp[4096];
        DWORD rd = 0;
        ReadFile(hStderrR, tmp, avail < sizeof(tmp) ? avail : (DWORD)sizeof(tmp), &rd, nullptr);
#else
        // read(tmp, 0, 4096) on a BufferedInputStream(8192) (ProcessPipeInputStream)
        if ((errBuf.size() - errPos) + (size_t)stderrAvailable() == 0) return;
        size_t want = 4096, got = 0;
        while (true) {
            if (errPos >= errBuf.size()) {
                errBuf.assign(8192, 0);
                errPos = 0;
                ssize_t n;
                do {
                    n = read(fdStderrR, &errBuf[0], errBuf.size());
                } while (n < 0 && errno == EINTR);
                errBuf.resize(n > 0 ? (size_t)n : 0);
                if (n <= 0) break;
            }
            size_t take = std::min(errBuf.size() - errPos, want - got);
            errPos += take;
            got += take;
            if (got >= want || stderrAvailable() <= 0) break;
        }
#endif
    }

    // Process.destroy(): kills the bot. The writer thread is left alone (it may be blocked on a pipe
    // a grandchild keeps open) and dies with the referee.
    void destroy() {
        stopWriter();
#ifdef _WIN32
        if (hProcess) {
            TerminateProcess(hProcess, 0);
            WaitForSingleObject(hProcess, 2000);
            CloseHandle(hProcess);
        }
        if (hStdoutR) CloseHandle(hStdoutR);
        if (hReadEvent) CloseHandle(hReadEvent);
        if (hStderrR) CloseHandle(hStderrR);
        hProcess = hStdoutR = hReadEvent = hStderrR = nullptr;
#else
        if (fdStdoutR >= 0) close(fdStdoutR);
        if (fdStderrR >= 0) close(fdStderrR);
        fdStdoutR = fdStderrR = -1;
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            int st;
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            }
            pid = -1;
        }
#endif
        outAlive = false;
    }

  private:
    AgentOptions opt;
    bool outAlive = false; // its stdout is not at EOF
    std::string readBuf;   // bytes read but not consumed yet
    size_t readPos = 0;
    bool lastCR = false;

    struct Writer {
        std::mutex m;
        std::condition_variable cv;
        std::deque<std::string> q;
        bool stop = false;
    };
    Writer* writer = nullptr; // never freed: the detached thread may outlive the game

#ifdef _WIN32
    typedef HANDLE Handle;
    HANDLE hProcess = nullptr;
    HANDLE hStdinW = nullptr;
    HANDLE hStdoutR = nullptr;   // overlapped, for reads with a precise timeout
    HANDLE hReadEvent = nullptr; // manual-reset event of the overlapped reads
    HANDLE hStderrR = nullptr;   // Stderr::Jar only

    bool spawn(const std::string& cmd) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
        if (!CreatePipe(&inR, &inW, &sa, 0)) return false;
        auto closeAll = [&] {
            for (HANDLE h : {inR, inW, outR, outW, errR, errW})
                if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        };
        if (opt.stderrMode == Stderr::Jar) {
            if (!CreatePipe(&errR, &errW, &sa, 4096 + 24)) {
                closeAll();
                return false;
            }
            SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
        } else if (opt.stderrMode == Stderr::Discard) {
            errW = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, nullptr);
            if (errW == INVALID_HANDLE_VALUE) errW = nullptr;
        } else if (opt.stderrMode == Stderr::File) {
            errW = CreateFileA(opt.stderrFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (errW == INVALID_HANDLE_VALUE) errW = nullptr;
        }
        // CreatePipe can't make an overlapped pipe: build the stdout pair with a unique named pipe
        char pipeName[96];
        static volatile LONG s_pipeCounter = 0;
        LONG pipeId = InterlockedIncrement(&s_pipeCounter);
        snprintf(pipeName, sizeof(pipeName), "\\\\.\\pipe\\battle_referee_%lu_%ld", (unsigned long)GetCurrentProcessId(), (long)pipeId);
        outR = CreateNamedPipeA(pipeName, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, &sa);
        if (outR == INVALID_HANDLE_VALUE) {
            outR = nullptr;
            closeAll();
            return false;
        }
        outW = CreateFileA(pipeName, GENERIC_WRITE, 0, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (outW == INVALID_HANDLE_VALUE) {
            outW = nullptr;
            closeAll();
            return false;
        }
        SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = inR;
        si.hStdOutput = outW;
        si.hStdError = errW ? errW : GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION pi{};
        // '/' -> '\' so cmd doesn't mistake path separators for option prefixes
        std::string full = cmd;
        for (auto& ch : full)
            if (ch == '/') ch = '\\';
        if (cmd.find_first_of("&|<>^()") != std::string::npos) full = "cmd /c \"" + full + "\"";
        std::vector<char> cmdline(full.begin(), full.end());
        cmdline.push_back(0);
        BOOL ok = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(inR);
        CloseHandle(outW);
        if (errW) CloseHandle(errW);
        inR = outW = errW = nullptr;
        if (!ok) {
            closeAll();
            return false;
        }
        CloseHandle(pi.hThread);
        hReadEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!hReadEvent) {
            closeAll();
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hProcess);
            return false;
        }
        hProcess = pi.hProcess;
        hStdinW = inW;
        hStdoutR = outR;
        hStderrR = errR;
        outAlive = true;
        return true;
    }

    static bool rawWrite(Handle h, const std::string& s) {
        size_t off = 0;
        DWORD wr;
        while (off < s.size()) {
            if (!WriteFile(h, s.data() + off, (DWORD)(s.size() - off), &wr, nullptr) || wr == 0) return false;
            off += wr;
        }
        return true;
    }

    // Reads more bytes into readBuf. False on timeout, EOF or error.
    bool fill(std::chrono::steady_clock::time_point deadline) {
        auto now = std::chrono::steady_clock::now();
        if (!outAlive || now >= deadline) return false;
        DWORD ms = (DWORD)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        if (ms < 1) ms = 1;
        char tmp[4096];
        OVERLAPPED ov{};
        ov.hEvent = hReadEvent;
        ResetEvent(hReadEvent);
        DWORD rd = 0;
        BOOL ok = ReadFile(hStdoutR, tmp, (DWORD)sizeof(tmp), &rd, &ov);
        if (!ok) {
            if (GetLastError() != ERROR_IO_PENDING) {
                outAlive = false;
                return false;
            }
            DWORD wr = WaitForSingleObject(hReadEvent, ms);
            if (wr == WAIT_TIMEOUT) {
                // time is up: cancel the read; what it got before the cancellation stays buffered, as
                // it would stay in the jar's pipe
                CancelIoEx(hStdoutR, &ov);
                DWORD got = 0;
                if (GetOverlappedResult(hStdoutR, &ov, &got, TRUE) && got > 0) readBuf.append(tmp, got);
                return false;
            }
            if (wr != WAIT_OBJECT_0 || !GetOverlappedResult(hStdoutR, &ov, &rd, FALSE)) {
                outAlive = false;
                return false;
            }
        }
        if (rd == 0) {
            outAlive = false;
            return false;
        }
        readBuf.append(tmp, rd);
        return true;
    }

    Handle inputHandle() const { return hStdinW; }
#else
    typedef int Handle;
    pid_t pid = -1;
    int fdStdinW = -1;
    int fdStdoutR = -1;
    int fdStderrR = -1;  // Stderr::Jar only
    std::string errBuf; // the BufferedInputStream of the bot's stderr
    size_t errPos = 0;

    bool spawn(const std::string& cmd) {
        signal(SIGPIPE, SIG_IGN);
        int inPipe[2], outPipe[2], errPipe[2] = {-1, -1};
        if (pipe(inPipe) != 0) return false;
        if (pipe(outPipe) != 0) {
            close(inPipe[0]);
            close(inPipe[1]);
            return false;
        }
        int errFd = -1;
        if (opt.stderrMode == Stderr::Jar) {
            if (pipe(errPipe) != 0) {
                close(inPipe[0]);
                close(inPipe[1]);
                close(outPipe[0]);
                close(outPipe[1]);
                return false;
            }
            errFd = errPipe[1];
        } else if (opt.stderrMode == Stderr::Discard) {
            errFd = open("/dev/null", O_WRONLY);
        } else if (opt.stderrMode == Stderr::File) {
            errFd = open(opt.stderrFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        }
        pid_t c = fork();
        if (c < 0) {
            for (int fd : {inPipe[0], inPipe[1], outPipe[0], outPipe[1], errPipe[0], errFd})
                if (fd >= 0) close(fd);
            return false;
        }
        if (c == 0) {
            dup2(inPipe[0], STDIN_FILENO);
            dup2(outPipe[1], STDOUT_FILENO);
            if (errFd >= 0) dup2(errFd, STDERR_FILENO);
            for (int fd : {inPipe[0], inPipe[1], outPipe[0], outPipe[1], errPipe[0], errFd})
                if (fd >= 0) close(fd);
            if (cmd.find_first_of("&|<>^()") != std::string::npos) {
                execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
            } else {
                wordexp_t w{};
                if (wordexp(cmd.c_str(), &w, WRDE_NOCMD) != 0 || w.we_wordc == 0) _exit(127);
                execvp(w.we_wordv[0], w.we_wordv);
            }
            _exit(127);
        }
        if (errFd >= 0) close(errFd);
        close(inPipe[0]);
        close(outPipe[1]);
        fdStdinW = inPipe[1];
        fdStdoutR = outPipe[0];
        fdStderrR = errPipe[0];
        pid = c;
        outAlive = true;
        return true;
    }

    static bool rawWrite(Handle fd, const std::string& s) {
        size_t off = 0;
        while (off < s.size()) {
            ssize_t n;
            do {
                n = write(fd, s.data() + off, s.size() - off);
            } while (n < 0 && errno == EINTR);
            if (n <= 0) return false;
            off += (size_t)n;
        }
        return true;
    }

    // Reads more bytes into readBuf. False on timeout, EOF or error.
    bool fill(std::chrono::steady_clock::time_point deadline) {
        for (;;) {
            auto now = std::chrono::steady_clock::now();
            if (!outAlive || now >= deadline) return false;
            int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            if (ms < 1) ms = 1;
            struct pollfd pfd;
            pfd.fd = fdStdoutR;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, ms);
            if (pr < 0 && errno == EINTR) continue;
            if (pr == 0) return false; // timeout
            if (pr < 0) {
                outAlive = false;
                return false;
            }
            char tmp[4096];
            ssize_t rd;
            do {
                rd = read(fdStdoutR, tmp, sizeof(tmp));
            } while (rd < 0 && errno == EINTR);
            if (rd <= 0) {
                outAlive = false;
                return false;
            }
            readBuf.append(tmp, (size_t)rd);
            return true;
        }
    }

    int stderrAvailable() const {
        int n = 0;
        if (fdStderrR < 0 || ioctl(fdStderrR, FIONREAD, &n) < 0) return 0;
        return n;
    }

    Handle inputHandle() const { return fdStdinW; }
#endif

    void startWriter() {
        writer = new Writer();
        Writer* w = writer;
        Handle h = inputHandle();
        std::thread([w, h] {
            std::unique_lock<std::mutex> lk(w->m);
            while (true) {
                w->cv.wait(lk, [w] { return w->stop || !w->q.empty(); });
                if (w->stop) break;
                std::string s = std::move(w->q.front());
                w->q.pop_front();
                lk.unlock();
                bool ok = rawWrite(h, s);
                lk.lock();
                if (!ok) break;
            }
        }).detach();
    }

    void stopWriter() {
        if (!writer) return;
        {
            std::lock_guard<std::mutex> g(writer->m);
            writer->stop = true;
            writer->q.clear();
        }
        writer->cv.notify_one();
    }
};

} // namespace cg
