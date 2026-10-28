package com.codingame.gameengine.runner;

import java.io.OutputStream;
import java.io.PrintStream;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.charset.Charset;
import java.nio.file.Paths;
import java.util.List;

import org.apache.commons.cli.CommandLine;
import org.apache.commons.cli.DefaultParser;
import org.apache.commons.cli.HelpFormatter;
import org.apache.commons.cli.Options;

import com.codingame.gameengine.runner.simulate.GameResult;
import com.google.common.io.Files;

/**
 * The command line interface of the jars of pre-SDK CodinGame referees (legacy/build.sh): the game runs in the
 * SDK 4.5.0 MultiplayerGameRunner whose referee agent is a LegacyRefereeAgent (the old referee's protocol is the
 * one the runner still speaks). Same options and output as CLI/CommandLineInterface-v4.java: one score per player,
 * then the game parameters (the lines of the referee's UINPUT command, i.e. its getConfiguration()).
 * <p>
 * The referee's own prints to System.out are discarded, so that stdout only gets the scores and the game
 * parameters (the runner copies System.out to the real stdout). The process exits at the end, whatever threads the
 * referee leaves behind (e.g. a referee thread that is still blocked).
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

            PrintStream stdout = System.out;
            System.setOut(new PrintStream(new OutputStream() {
                @Override
                public void write(int b) {
                }
            }));

            // Launch Game
            gameRunner = new MultiplayerGameRunner();
            gameRunner.setLeagueLevel(19); // max (old referees take their league from the init properties or a constant)

            // the old referee instead of RefereeMain
            Field referee = GameRunner.class.getDeclaredField("referee");
            referee.setAccessible(true);
            referee.set(gameRunner, new LegacyRefereeAgent());

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
                        gameRunner.addAgent(commandLine, commandLine);
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

            for (String line : result.gameParameters) {
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

        System.exit(exitCode);
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
        List<Agent> players = gameRunner.players;

        if (players != null) {
            for (Agent player : players) {
                player.destroy();
            }
        }
    }

}
