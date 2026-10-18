package com.codingame.gameengine.runner;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.charset.Charset;
import java.nio.file.Paths;
import java.util.List;
import java.util.Properties;

import org.apache.commons.cli.CommandLine;
import org.apache.commons.cli.DefaultParser;
import org.apache.commons.cli.HelpFormatter;
import org.apache.commons.cli.Options;

import com.google.common.io.Files;

/**
 * Same interface as CommandLineInterface-v4, for games built on the CodinGame engine 1.x.
 *
 * Engine 1.x has no MultiplayerGameRunner nor GameRunner#simulate, so the game is driven through
 * the private GameRunner#initialize and GameRunner#run methods. The league level is read by the
 * engine from the "league.level" system property.
 */
public class CommandLineInterface {

    public static void main(String[] args) {
        GameRunner gameRunner = null;
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
                    .addOption("seed", true, "Referee seed");

            CommandLine cmd = new DefaultParser().parse(options, args);

            if (cmd.hasOption("h") || !cmd.hasOption("p1") || !cmd.hasOption("p2")) {
                new HelpFormatter().printHelp(
                        "-p1 <player1 command line> -p2 <player2 command line> [-p3 <cmd> -p4 <cmd> -l <log file> -seed <seed>]",
                        options);
                System.exit(0);
            }

            // Launch Game
            if (System.getProperty("league.level") == null) {
                System.setProperty("league.level", "19"); // max
            }

            Properties properties = new Properties();
            if (cmd.hasOption("seed")) {
                properties.setProperty("seed", String.valueOf(Long.parseLong(cmd.getOptionValue("seed"))));
            } else {
                properties.setProperty("seed", String.valueOf(System.nanoTime() + new Object().hashCode()));
            }

            gameRunner = new GameRunner(properties);

            int playerCount = 0;

            for (int i = 1; i <= 4; ++i) {
                if (cmd.hasOption("p" + i)) {
                    gameRunner.addAgent(cmd.getOptionValue("p" + i));
                    playerCount += 1;
                }
            }

            Method initialize = GameRunner.class.getDeclaredMethod("initialize", Properties.class);
            initialize.setAccessible(true);
            initialize.invoke(gameRunner, new Properties());

            Method run = GameRunner.class.getDeclaredMethod("run");
            run.setAccessible(true);
            run.invoke(gameRunner);

            if (cmd.hasOption("l")) {
                Method getJSONResult = GameRunner.class.getDeclaredMethod("getJSONResult");
                getJSONResult.setAccessible(true);

                Files.asCharSink(Paths.get(cmd.getOptionValue("l")).toFile(), Charset.defaultCharset())
                        .write((String) getJSONResult.invoke(gameRunner));
            }

            for (int i = 0; i < playerCount; ++i) {
                System.out.println(gameRunner.gameResult.scores.get(i));
            }

            for (String line : gameRunner.gameResult.uinput) {
                System.out.println(line);
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

    private static void destroyPlayerProcesses(GameRunner gameRunner) {
        // CommandLinePlayerAgent#destroy is a no-op in engine 1.x, so kill the processes ourselves
        try {
            Field playersField = GameRunner.class.getDeclaredField("players");
            playersField.setAccessible(true);
            Field processField = CommandLinePlayerAgent.class.getDeclaredField("process");
            processField.setAccessible(true);

            for (Object player : (List<?>) playersField.get(gameRunner)) {
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
