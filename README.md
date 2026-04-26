# `battle` bot tools

Tools for building competitive bots, e.g. for [CodinGame](https://codingame.com) contests.

The project is composed of [multiple Rust crates](crates/README.md), conveniently accesible under the same CLI tool called `battle`. The tools are designed to "just work" and aim to have good DX. 🛠️ Still a lot to do.

## Install

Clone and run:

```sh
cargo install --path crates/battle
```

To update, pull the latest changes and run the commands again.

## Usage / Cheatsheet

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
    <td rowspan="2"><code>battle cgsync</code></br></br>
Watches your project, bundles on every change, and pushes the result to the CodinGame browser IDE through the <a href="https://github.com/jmerle/cg-local-ext">CG Local extension</a>. See <a href="crates/cgsync">cgsync</a>.</td>
    <td><code>battle cgsync</code></td>
    <td>Auto-detects the project in the current folder. Click the browser extension to attach after starting.</td>
  </tr>
  <tr>
    <td><code>battle cgsync src/main.cpp</code></td>
    <td>Specify the entrypoint manually (file or folder).</td>
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
Records and later plays back a command's stdin / stdout / stderr. See <a href="crates/wrapcmd">wrapcmd</a>.</td>
    <td><code>battle wrap capture out.txt -- ./bot</code></td>
    <td>Run <code>./bot</code> transparently while writing a transcript to <code>out.txt</code>.</td>
  </tr>
  <tr>
    <td><code>battle wrap playback out.txt</code></td>
    <td>Plays back the transcript: writes the recorded stdout/stderr and validates that stdin matches.</td>
  </tr>
</table>


## Tips

* Make aliases for common commands: `battle build` -> `bb`
* Pass the referee as an environment variable to skip writing it each time: `-r some-game` -> `BATTLE_REFEREE=some-game`
* Keep your worker list in an environment variable: `BATTLE_WORKERS=100.100.1.1,100.100.1.2,etc`
