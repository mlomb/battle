package com.codingame.gameengine.runner;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.PrintStream;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.nio.file.Paths;
import java.util.List;

import org.apache.commons.cli.CommandLine;
import org.apache.commons.cli.DefaultParser;
import org.apache.commons.cli.HelpFormatter;
import org.apache.commons.cli.Options;

import com.codingame.gameengine.runner.dto.GameResult;
import com.google.common.io.Files;

/**
 * The v4 command line interface for engines 2.x (e.g. Xmas Rush, engine 2.15, and Legends of Code & Magic,
 * engine 2.5): GameRunner#simulate returns a
 * dto.GameResult whose game parameters are in `uinput` (as in CommandLineInterface-v3.java), but
 * MultiplayerGameRunner has no setLeagueLevel (the engine reads the "league.level" system property, as in
 * CommandLineInterface-v1.java), no addAgent(commandLine, nickname), and CommandLinePlayerAgent#destroy does not
 * kill the bot. Same options and output as CommandLineInterface-v4.java.
 * <p>
 * The league is 19 (the highest league of most games), unless the jar was built for another one: build.sh's
 * LEAGUE_LEVEL puts it in the cg-builder/league.level resource (e.g. Ultimate Tic-Tac-Toe, engine 2.7, whose
 * referee only knows its levels 1 and 2).
 */
public class CommandLineInterface {

    public static void main(String[] args) {
        MultiplayerGameRunner gameRunner = null;
        int exitCode = 0;

        try {
            Options options = new Options();

            // Define required options
            options.addOption("h", false, "Print the help")
                    .addOption("p1", true, "Required. Player 1 command line")
                    .addOption("p2", true, "Required. Player 2 command line")
                    // it is the responsibility of the caller to know if the game supports 2, 3 or 4 players
                    .addOption("p3", true, "Player 3 command line (if applies)")
                    .addOption("p4", true, "Player 4 command line (if applies)")
                    .addOption("l", true, "File output for logs")
                    .addOption("seed", true, "Referee seed")
                    .addOption("timeout", true, "Turn time limit in ms, replacing the game's (first turn: the longest of both)");

            CommandLine cmd = new DefaultParser().parse(options, args);

            if (cmd.hasOption("h") || !cmd.hasOption("p1") || !cmd.hasOption("p2")) {
                new HelpFormatter().printHelp(
                        "-p1 <player1 command line> -p2 <player2 command line> [-p3 <cmd> -p4 <cmd> -l <log file> -seed <seed> -timeout <ms>]",
                        options);
                System.exit(0);
            }

            // The referee's own prints to System.out are discarded, as CodinGame's RefereeMain#main does:
            // stdout only gets the scores and the game parameters (e.g. Legends of Code & Magic, engine 2.5,
            // prints its turns there, and the runners of engines 2.15+ copy the referee's prints to stdout)
            PrintStream stdout = System.out;
            System.setOut(new PrintStream(new OutputStream() {
                @Override
                public void write(int b) {
                }
            }));

            // Launch Game
            if (System.getProperty("league.level") == null) {
                System.setProperty("league.level", builtLeagueLevel());
            }

            gameRunner = new MultiplayerGameRunner();

            if (cmd.hasOption("seed")) {
                Long seed = Long.parseLong(cmd.getOptionValue("seed"));
                gameRunner.setSeed(seed);
            } else {
                gameRunner.setSeed(System.nanoTime() + new Object().hashCode());
            }


            Long turnTimeout = cmd.hasOption("timeout") ? Long.parseLong(cmd.getOptionValue("timeout")) : null;

            // the private MultiplayerGameRunner#addAgent(Agent, nickname, avatar) the public ones use
            Method addAgent = MultiplayerGameRunner.class.getDeclaredMethod("addAgent", Agent.class, String.class, String.class);
            addAgent.setAccessible(true);

            int playerCount = 0;

            for (int i = 1; i <= 4; ++i) {
                if (cmd.hasOption("p" + i)) {
                    String commandLine = cmd.getOptionValue("p" + i);
                    if (turnTimeout != null) {
                        addAgent.invoke(gameRunner, new TimeoutOverridePlayerAgent(commandLine, turnTimeout), commandLine, null);
                    } else {
                        gameRunner.addAgent(commandLine, commandLine, null);
                    }
                    playerCount += 1;
                }
            }

            GameResult result = gameRunner.simulate();

            if (cmd.hasOption("l")) {
                Method getJSONResult = GameRunner.class.getDeclaredMethod("getJSONResult");
                getJSONResult.setAccessible(true);

                Files.asCharSink(Paths.get(cmd.getOptionValue("l")).toFile(), Charset.defaultCharset())
                        .write((String) getJSONResult.invoke(gameRunner));
            }

            for (int i = 0; i < playerCount; ++i) {
                stdout.println(result.scores.get(i));
            }

            for (String line : result.uinput) {
                stdout.println(line);
            }

        } catch (Exception e) {
            System.err.println(e);
            e.printStackTrace(System.err);
            exitCode = 1;
        } finally {
            if (gameRunner != null) {
                destroyPlayerProcesses(gameRunner);
            }
        }

        // the engine leaves non-daemon threads behind, make sure we exit
        System.exit(exitCode);
    }

    /**
     * The league level the jar was built for (build.sh's LEAGUE_LEVEL), 19 (max) by default.
     */
    private static String builtLeagueLevel() throws IOException {
        InputStream in = CommandLineInterface.class.getResourceAsStream("/cg-builder/league.level");
        if (in == null) {
            return "19"; // max
        }
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(in, StandardCharsets.UTF_8))) {
            return reader.readLine().trim();
        }
    }

    /**
     * A command line player whose turn time limit is replaced by the -timeout one, except that a first turn
     * keeps the game's limit when it is longer. Same rule as turnTimeoutMs in the C++ referees.
     */
    static class TimeoutOverridePlayerAgent extends CommandLinePlayerAgent {
        private final long turnTimeout;
        private boolean executedOnce = false;

        TimeoutOverridePlayerAgent(String commandLine, long turnTimeout) {
            super(commandLine);
            this.turnTimeout = turnTimeout;
        }

        @Override
        public String getOutput(int nbLine, long timeout) {
            boolean firstTurn = nbLine > 0 && !executedOnce;
            if (nbLine > 0) {
                executedOnce = true;
            }
            return super.getOutput(nbLine, firstTurn ? Math.max(timeout, turnTimeout) : turnTimeout);
        }
    }

    private static void destroyPlayerProcesses(MultiplayerGameRunner gameRunner) {
        // CommandLinePlayerAgent#destroy is a no-op in engine 2.x, so kill the processes ourselves
        try {
            Field processField = CommandLinePlayerAgent.class.getDeclaredField("process");
            processField.setAccessible(true);

            for (Agent player : gameRunner.players) {
                if (player instanceof CommandLinePlayerAgent) {
                    Process process = (Process) processField.get(player);
                    if (process != null) {
                        process.destroyForcibly();
                    }
                }
            }
        } catch (ReflectiveOperationException e) {
            e.printStackTrace(System.err);
        }
    }

}
