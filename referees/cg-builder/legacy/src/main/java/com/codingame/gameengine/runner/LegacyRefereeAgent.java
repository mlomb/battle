package com.codingame.gameengine.runner;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.PipedInputStream;
import java.io.PipedOutputStream;
import java.io.PrintStream;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

/**
 * The referee agent of a pre-SDK CodinGame referee: a copy of the SDK 4.5.0 RefereeAgent (same pipes, same
 * getOutput, so the runner reads the referee exactly as it reads a GameManager), whose thread runs
 * {@code new Referee(stdin, stdout, stderr)} instead of {@code RefereeMain.start(stdin, stdout)}: the constructor of
 * the old AbstractReferee runs the whole protocol loop. CommandLineInterface installs it in the
 * MultiplayerGameRunner (its private field {@code referee}). A referee without that constructor (Game of Drones,
 * which has no base class) is run by its other entry point instead: see runStart.
 * <p>
 * The game's {@code Referee} class is in the default package (its source is copied as is), hence the reflection.
 * <p>
 * The referee's stderr stream (AbstractReferee#printError) is the runner's System.err: nothing reads the agent's
 * stderr pipe of RefereeAgent, which would block a referee printing more than 100 KB there.
 */
class LegacyRefereeAgent extends Agent {

    public static final int REFEREE_MAX_BUFFER_SIZE_EXTRA = 100_000;
    public static final int REFEREE_MAX_BUFFER_SIZE = 30_000;
    private boolean lastRefereeByteIsCarriageReturn = false;

    private PipedInputStream agentStdin = new PipedInputStream(100_000);
    private PipedOutputStream agentStdout = new PipedOutputStream();
    private PipedOutputStream agentStderr = new PipedOutputStream();

    private OutputStream processStdin = null;
    private InputStream processStdout = null;
    private InputStream processStderr = null;

    private Thread thread;

    public LegacyRefereeAgent() {
        super();

        try {
            processStdin = new PipedOutputStream(agentStdin);
            processStdout = new PipedInputStream(agentStdout, 100_000);
            processStderr = new PipedInputStream(agentStderr, 100_000);
        } catch (IOException e) {
            throw new RuntimeException("Cannot initialize Referee Agent");
        }
    }

    @Override
    public void destroy() {
        if (thread != null) {
            thread.interrupt();
        }
    }

    @Override
    protected OutputStream getInputStream() {
        return processStdin;
    }

    @Override
    protected InputStream getOutputStream() {
        return processStdout;
    }

    @Override
    protected InputStream getErrorStream() {
        return processStderr;
    }

    @Override
    protected void runInputOutput() throws Exception {
        final PrintStream err = System.err;
        thread = new Thread() {
            public void run() {
                try {
                    Class<?> referee = Class.forName("Referee");
                    Constructor<?> constructor;
                    try {
                        constructor = referee.getDeclaredConstructor(InputStream.class, PrintStream.class, PrintStream.class);
                    } catch (NoSuchMethodException e) {
                        runStart(referee);
                        return;
                    }
                    constructor.setAccessible(true);
                    constructor.newInstance(agentStdin, new PrintStream(agentStdout), err);
                } catch (InvocationTargetException e) {
                    // what the referee's constructor threw: the thread dies like it would have
                    e.getCause().printStackTrace();
                } catch (ReflectiveOperationException e) {
                    throw new RuntimeException("Cannot start the legacy referee", e);
                }
            }
        };
        thread.start();
    }

    /**
     * The second entry point, for a referee without the base's constructor: Game of Drones' {@code Referee} has no
     * base class; its {@code main} runs {@code new Referee().start(System.in)} and it prints its commands to
     * {@code System.out}. Here (in the referee's thread, like the constructor above): System.out becomes the stdout
     * pipe for the rest of the run, then the no-argument constructor and {@code start(stdin)}. CommandLineInterface
     * keeps its own reference to the real stdout for the scores and the game parameters, and nothing else prints to
     * System.out during the game. {@code start} never returns (after the scores it waits for the next command): the
     * thread ends with the stream (the exception is printed like the constructor's above), the CLI exits at the end.
     */
    void runStart(Class<?> refereeClass) throws ReflectiveOperationException {
        System.setOut(new PrintStream(agentStdout));
        Constructor<?> constructor = refereeClass.getDeclaredConstructor();
        constructor.setAccessible(true);
        Object referee = constructor.newInstance();
        Method start = refereeClass.getDeclaredMethod("start", InputStream.class);
        start.setAccessible(true);
        start.invoke(referee, agentStdin);
    }

    @Override
    public String getOutput(int nbLine, long timeout) {
        return getOutput(nbLine, timeout, false);
    }

    @Override
    public String getOutput(int nbLine, long timeout, boolean extraBufferSpace) {
        if (processStdout == null) {
            return null;
        }
        int maxBufferSize = extraBufferSpace ? REFEREE_MAX_BUFFER_SIZE_EXTRA : REFEREE_MAX_BUFFER_SIZE;
        try {
            byte[] tmp = new byte[maxBufferSize];
            int offset = 0;
            int nbOccurences = 0;

            long t0 = System.nanoTime();

            while ((offset < maxBufferSize) && (nbOccurences < nbLine)) {
                long current = System.nanoTime();
                if ((current - t0) > (timeout * 1_000_000L)) {
                    break;
                }

                while ((offset < maxBufferSize) && (processStdout.available() > 0)
                        && (nbOccurences < nbLine)) {
                    current = System.nanoTime();
                    if ((current - t0) > (timeout * 1_000_000L)) {
                        break;
                    }

                    int nbRead = processStdout.read(tmp, offset, 1);
                    if (nbRead <= 0) {
                        break;
                    }
                    byte curByte = tmp[offset];
                    if (!((curByte == '\n') && lastRefereeByteIsCarriageReturn)) {
                        offset += nbRead;
                        if ((curByte == '\n') || (curByte == '\r')) {
                            ++nbOccurences;
                        }
                    }
                    lastRefereeByteIsCarriageReturn = curByte == '\r';
                }

                if (!((offset < REFEREE_MAX_BUFFER_SIZE) && (nbOccurences < nbLine))) {
                    break;
                }

                try {
                    Thread.sleep(1);
                } catch (Exception e) {
                }
            }
            return new String(tmp, 0, offset, UTF8);
        } catch (IOException e) {
            e.printStackTrace();
            processStdout = null;
        }
        return null;
    }
}
