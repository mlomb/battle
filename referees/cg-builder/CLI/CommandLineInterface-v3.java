package com.codingame.gameengine.runner;

import java.lang.reflect.Method;
import java.nio.charset.Charset;
import java.nio.file.Paths;
import java.util.List;

import org.apache.commons.cli.CommandLine;
import org.apache.commons.cli.DefaultParser;
import org.apache.commons.cli.HelpFormatter;
import org.apache.commons.cli.Options;

import com.codingame.gameengine.runner.dto.GameResult;
import com.google.common.io.Files;

/**
 * The v4 command line interface for engines 3.x older than 3.15.2 (e.g. Spring Challenge 2020, engine 3.15.0):
 * there GameRunner#simulate returns a dto.GameResult whose game parameters are in `uinput` (the simulate
 * package came with 3.15.2). Same options and output as CommandLineInterface-v4.java.
 */
public class CommandLineInterface {

    public static void main(String[] args) {
        MultiplayerGameRunner gameRunner = null;

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

            // Launch Game
            gameRunner = new MultiplayerGameRunner();
            gameRunner.setLeagueLevel(19); // max

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
                System.out.println(result.scores.get(i));
            }

            for (String line : result.uinput) {
                System.out.println(line);
            }

        } catch (Exception e) {
            System.err.println(e);
            e.printStackTrace(System.err);
            System.exit(1);
        } finally {
            if (gameRunner != null) {
                destroyPlayerProcesses(gameRunner);
            }
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
        List<Agent> players = gameRunner.players;

        if (players != null) {
            for (Agent player : players) {
                player.destroy();
            }
        }
    }

}
