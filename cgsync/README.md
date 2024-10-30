# cgsync

[CodinGame](https://www.codingame.com/multiplayer/bot-programming) is a platform that hosts bot programming contests. The platform provides a browser-based IDE for writing code and running games. Writing code in the browser lacks many features (like a debugger) and is very cumbersome when bots become large.

A better approach is to write your code in your desktop IDE and sync the code into the browser IDE when files are saved to disk. [CG Local](https://github.com/jmerle/cg-local-app) is an app and browser extension made by [Jasper van Merle](https://github.com/jmerle) that does this. 
However the application requires Java (which is an oof for me) and is GUI-based. Also, it does not support inclusion resolution.

`cgsync` does the same as CG Local App, but as a command-line tool. More importantly, it uses the `bundler` crate to bundle the code so it supports its features (multiple files, etc.). The [CG Local Browser Extension](https://github.com/jmerle/cg-local-ext) is required for `cgsync` to work since it uses the same protocol as the CG Local App.

## Usage

You need to install the [CG Local Extension](https://github.com/jmerle/cg-local-ext) first:

[link-cws]: https://chromewebstore.google.com/detail/cg-local/ihakjfajoihlncbnggmcmmeabclpfdgo
[link-amo]: https://addons.mozilla.org/en-US/firefox/addon/cg-local/

- [**Chrome** extension][link-cws] [<img valign="middle" src="https://img.shields.io/chrome-web-store/v/ihakjfajoihlncbnggmcmmeabclpfdgo.svg?label=%20">][link-cws]
- [**Firefox** add-on][link-amo] [<img valign="middle" src="https://img.shields.io/amo/v/cg-local.svg?label=%20">][link-amo]

Then use `cgsync` the same way you would use `bundler`. Open the folder of your C++ or Rust project and run:

```sh
cgsync
```

When changes are made, `cgsync` will bundle and send the code to the browser.

## How it works

The process starts a local web socket server on port 53135. The browser extension then connects to this server. When a file changes the code is bundled using `bundler` and sent to the browser extension, which pastes it into the browser IDE.

