# Bot tools

Tools for building competitive bots. The project is composed of [multiple Rust crates](crates/README.md), conviniently accesible under the same CLI tool called `battle`.

The tools are designed to "just work" and aim to have good DX.

🛠️ **Still work in progress**

## Install

Clone and run:

```sh
cargo install --path crates/battle
```

To update, pull the latest changes and run the commands again.

## Usage

<table>
  <tr>
    <th>Command</th>
    <th>Examples</th>
    <th>Note</th>
  </tr>
  <tr>
    <td rowspan="3"><code>battle bundle</code></br></br>
A tool to bundle C++ and Rust projects into a single source unit for submission.</td>
    <td><code>battle bundle</code></td>
    <td>It will look for a C++ or Rust project (see <a href="crates/bundler/README.md#what-is-a-project">What is a project?</a>) and print a single file to stdout.</td>
  </tr>
  <tr>
    <td><code>battle build v5.cpp</code></td>
    <td>Specify entrypoint manually</td>
  </tr>
  <tr>
    <td><code>battle build main.cpp --output submission.cpp</code></td>
    <td>Output to a file instead of stdout.</td>
  </tr>
  <tr>
    <td><code>battle build</code></br></br>A tool to bundle and build your bot and check compilation issues.</td>
    <td><code>battle build</code></td>
    <td>Same as <code>bundle</code>, it will try to find an entrypoint automaticaly.</td>
  </tr>
  <tr>
    <td rowspan="2"><code>battle worker</code></br></br>
Starts a worker node that the distributed runner uses to play games.</td>
    <td><code>battle worker</code></td>
    <td>Listens on the default port using <em>physical CPUs − 2</em> threads.</td>
  </tr>
  <tr>
    <td><code>battle worker --threads 32 --port 8080</code></td>
    <td>Override the thread count and listening port.</td>
  </tr>
  <tr>
    <td rowspan="2"><code>battle play</code></br></br>
Plays one or more games between bots through the worker pool, streaming results as they finish.</td>
    <td><code>battle play -r cg-fall-2023-fish -a v1.cpp -a v2.cpp</code></td>
    <td>Play a single game with the given referee and agents (repeat <code>-a</code> per agent).</td>
  </tr>
  <tr>
    <td><code>battle play -r ... -a v1.cpp -a v2.cpp -n 100</code></td>
    <td>Play <code>-n</code> games in total.</td>
  </tr>
  <tr>
    <td rowspan="2"><code>battle referee-diff</code></br></br>
Runs the same game on a reference and a candidate referee and stops at the first score / status mismatch. Useful when porting or optimizing a referee while preserving outcomes.</td>
    <td><code>battle referee-diff --reference ref-impl --candidate cand-impl -a main.cpp</code></td>
    <td>Compare both referees using the given agent(s). Use a non-trivial, non-deterministic agent for meaningful comparisons.</td>
  </tr>
  <tr>
    <td><code>battle referee-diff ... --max-games 50</code></td>
    <td>Run up to <code>--max-games</code> identical games before declaring success (default <code>10</code>).</td>
  </tr>
  <tr>
    <td rowspan="2"><code>battle wrap</code></br></br>
Records and later replays a command's stdin / stdout / stderr. See <a href="crates/wrapcmd">wrapcmd</a>.</td>
    <td><code>battle wrap capture out.txt -- ./bot</code></td>
    <td>Run <code>./bot</code> transparently while writing a transcript to <code>out.txt</code>.</td>
  </tr>
  <tr>
    <td><code>battle wrap replay out.txt</code></td>
    <td>Replays the transcript: writes the recorded stdout/stderr and validates that stdin matches.</td>
  </tr>
</table>


# Example workflow

This section explains the _expected/recommended_ workflow to use the tools. For more information and complex use cases, refer to the READMEs of each tool.

[link-cws]: https://chromewebstore.google.com/detail/cg-local/ihakjfajoihlncbnggmcmmeabclpfdgo
[link-amo]: https://addons.mozilla.org/en-US/firefox/addon/cg-local/

<details>
<summary>CodinGame</summary>
<br>
A new CodinGame contest huh?
<br>

1. Prepare a new **project folder** for you bot, you can use any build system you want. It can be a C++ or Rust project (see [What is a project?](bundler/README.md#what-is-a-project)).

2. Make sure you have the [CG Local Extension](https://github.com/jmerle/cg-local-ext#install) installed in your browser:  
    - [**Chrome** extension][link-cws] [<img valign="middle" src="https://img.shields.io/chrome-web-store/v/ihakjfajoihlncbnggmcmmeabclpfdgo.svg?label=%20">][link-cws]  
    - [**Firefox** add-on][link-amo] [<img valign="middle" src="https://img.shields.io/amo/v/cg-local.svg?label=%20">][link-amo]  

3. Run `cgsync` in the project folder. This will watch your code and sync it with the browser IDE. **After the command is run, click the extension to initiate the connection.**

4. Develop your bot.

5. Now, you want to test changes to your bot. You can create snapshots of your code running `bundler --output versions/v1.cpp` in the project folder. This gives you a self-contained file with the current code.

_In the future, you will be able to define parameters in the code and testing will be a lot easier._

6. Once you have multiple snapshots (versions or features), create a new **arena environment file** named `env.yaml` in the project folder. For example:  
```yml
referee: cg-fall-2023-fish # make sure to use the correct referee (read note below)

agents:
  latest: # name of the agent
    src: main.cpp
  v1:
    src: versions/v1.cpp
  some-feature:
    src: versions/sf.cpp
  # ... add every agent you want
```

7. Run the `arena` command in the folder that contains the `env.yaml` file. It will guide you through the process of constructing the proper command to run the tournament you want. For example:  
```sh
arena tournament --format round-robin -a latest -a v1
```
</details>

> [!NOTE]
> To use the `arena` command to test your bot, make sure a referee is available (see [arena/ENV.md](arena/ENV.md#available-referees)). TLDR, you have to wait until I push a build of the referee (and trust me) or build it yourself.
