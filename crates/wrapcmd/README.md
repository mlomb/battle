# wrapcmd

Wraps a child process so you can **record** everything that goes in and out on stdin, stdout, and stderr, then **play back** that session later. From the caller’s perspective, capture mode behaves like running the command directly: streams are forwarded while a transcript file is written.

This is used to implement agent I/O capturing and referee diffing in `battle`.

> [!WARNING]
> Only works when the I/O is UTF-8 only.

## Usage

### Capture

If you want to capture `./main a b c`, you would run:

```sh
battle wrap capture /tmp/trans.io ./main a b c
```

Then, you can check the file `/tmp/trans.io`, [the format is specified below](#transcript-format).

### Playback

Then, don't forget to pass the same data through stdin!

```sh
battle wrap playback /tmp/trans.io < stdin.txt
```

## Transcript format

The file is plain text: one event per line, `tag` + space + data. Newlines in the original stream are split into separate lines.

| Tag | Meaning   |
|-----|-----------|
| `<` | stdin     |
| `>` | stdout    |
| `!` | stderr    |

Example:

```text
> hello from stdout
! hello from stderr
< received from stdin
> a
! b
> goodbye from stdout
! goodbye from stderr
```

## How it works

Capture spawns the wrapped process with pipes, reads stdout and stderr on background threads, and interleaves stdin/stdout/stderr into the transcript as they occur. Each stream is handled **line by line**, so the recording is line-granular, not byte-granular.

Playback parses the transcript, consumes stdin from the invoker, checks it against recorded stdin events, and prints stdout and stderr in recorded order.

## Quick demo

```sh
# capture a small shell
echo "Alice" | battle wrap capture /tmp/demo.io bash -c 'read name; echo "hello, $name"; echo "done" >&2'

# inspect the transcript
cat /tmp/demo.io

# replay it
echo "Alice" | battle wrap playback /tmp/demo.io

# try wrong stdin
# > stdin mismatch, expected: Alice, got: Bob
echo "Bob" | battle wrap playback /tmp/demo.io
```
