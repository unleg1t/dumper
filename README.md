# jvmdump

Dump **every class file loaded by a live HotSpot JVM** — a native, single-binary
alternative to Java agents, `jcmd` and JDK-specific tooling.

* No Java agent (`-javaagent`) and no `Instrumentation` API.
* No JDK installed on the machine running the JVM, no helper JVM process.
* Talks to the JVM directly over HotSpot's dynamic attach socket and loads a
  tiny JVMTI shared library, exactly like a profiler would.
* Writes a ready-to-use `.class` tree that `javap`, CFR, Procyon, JADX or any
  other decompiler can read.

Works on Arch Linux (and any glibc Linux) with a stock `jdk-openjdk`.

---

## Install

### Arch Linux

```bash
sudo pacman -S --needed base-devel jdk-openjdk
git clone <this repo> jvmdump && cd jvmdump
make
sudo make install          # /usr/local/bin/jvmdump + /usr/local/lib/jvmdump/libjvmdump.so
```

`make` finds the JDK headers through `javac`/`JAVA_HOME` automatically
(including Arch's `/usr/lib/jvm/java-XX-openjdk`). If you prefer not to install
anything system-wide, just run the in-tree binary:

```bash
make
./bin/jvmdump <pid>
```

Requires: `glibc` at runtime. Building needs `gcc`/`make` and a JDK for
`jvmti.h` only — the agent does not link against `libjvm.so`.

---

## Usage

```bash
jvmdump [options] <pid>
```

```bash
$ jvmdump 1234
jvmdump: attaching to pid 1234 (/usr/local/lib/jvmdump/libjvmdump.so)
jvmdump: dumping classes to /home/me/jvmdump-1234-20261028-190512
jvmdump: wrote 697 class files (4.3 MiB) from 718 loaded classes, 28 failed
jvmdump: see /home/me/jvmdump-1234-20261028-190512/failed.txt for the classes that could not be dumped
/home/me/jvmdump-1234-20261028-190512
```

The output path is the only thing printed on stdout, so it is easy to script:

```bash
dir=$(jvmdump -q "$pid")
javap -p -c -cp "$dir" com.example.Main
```

### Common recipes

```bash
# everything, quietly, into a fixed directory
jvmdump -q -o /tmp/dump 1234

# only your own classes (skips the JDK)
jvmdump --no-jdk 1234

# only one package
jvmdump -i 'com.example.*' 1234

# several filters (repeatable, or comma separated)
jvmdump -i 'com.example.*,org.acme.*' -e '*.generated.*' 1234

# just the class names, one per line
jvmdump --list 1234 | grep -i controller

# flat file names, no package directories
jvmdump --flat --no-jdk 1234

# dump a JVM owned by another user (root switches to that uid automatically)
sudo jvmdump 4242
```

### Options

| Option | Description |
| --- | --- |
| `-o, --out DIR` | Output directory. Default: `./jvmdump-<pid>-<timestamp>`, or `/tmp/...` when root dumps another user's JVM. |
| `-i, --include GLOB` | Only dump classes whose dotted name matches `GLOB` (`*`, `?`). Repeatable / comma separated. |
| `-e, --exclude GLOB` | Skip matching classes. Repeatable / comma separated. |
| `--no-jdk` | Shorthand for excluding `java.*`, `javax.*`, `jdk.*`, `sun.*`, `com.sun.*`, `org.w3c.*`, `org.xml.*`. |
| `-l, --list` | Only list class names (`<out>/classes.txt`), no class files. |
| `-f, --flat` | Write `com.example.Foo.class` instead of `com/example/Foo.class`. |
| `-a, --agent PATH` | Path to `libjvmdump.so` (auto-detected next to the binary / in the install dir). |
| `--timeout SEC` | How long to wait for the attach listener to appear (default 10). |
| `-q, --quiet` | Print only the result path. |
| `-v, --verbose` | Print the full agent report and diagnostics. |
| `-V, --version`, `-h, --help` | |

### Output layout

```
<out>/
├── classes/                     one .class file per loaded class
│   ├── com/example/Main.class
│   └── ...
├── manifest.tsv                 class name <TAB> size <TAB> relative path
├── failed.txt                   classes that could not be dumped (if any)
└── dump.log                     machine readable report
```

List mode writes `<out>/classes.txt` instead of `classes/`.

`dump.log` looks like this:

```
jvmdump 1.0.0
pid: 1234
started: 2026-10-28 19:05:12
outdir: /tmp/dump
mode: dump
capabilities: retransform=1 retransform-any=1 all-class-hook-events=1
classes-loaded: 718
classes-selected: 610
classes-filtered: 108
files-written: 697
bytes-written: 4465352
duplicates: 0
unnamed: 0
failed-count: 28
elapsed: 0.33s
status: ok
```

`status: ok` means the dump completed; individual classes can still be listed in
`failed.txt` (see limitations below).

---

## How it works

1. `jvmdump` finds the JVM's attach socket (`<java.io.tmpdir>/.java_pid<pid>`),
   creates the `.attach_pid<pid>` trigger and sends `SIGQUIT` if the listener is
   not running yet, then speaks HotSpot's binary attach protocol
   (`"1\0load\0<path>\0true\0<options>\0"`). No JDK classes are involved.
2. The JVM `dlopen`s `libjvmdump.so` and calls `Agent_OnAttach`, which creates a
   JVMTI environment.
3. `GetLoadedClasses` enumerates every loaded class; `--include/--exclude`
   filters are applied to the dotted names.
4. The agent enables `can_retransform_classes` and a `ClassFileLoadHook`
   callback, then calls `RetransformClasses` in batches of 32. The hook receives
   the current class file bytes and writes them out. **The class data is never
   modified** — the hook only reads it, so no class is actually redefined.
5. Classes defined while the dump is running (for example lambda forms
   generated by the JVM) are captured too, because the hook is still installed.
6. The agent writes `manifest.tsv`, `failed.txt`, `dump.log` and the `.class`
   tree; the client prints the summary and exits.

### Why not `RetransformClasses` on everything at once?

A single unmodifiable class makes the whole `RetransformClasses` call fail. The
agent therefore retries a failed batch class by class, records the ones that
cannot be dumped in `failed.txt`, and keeps going.

---

## Limitations

* **Hidden classes cannot be dumped.** Classes generated with
  `MethodHandles.Lookup.defineHiddenClass` (lambdas, method-handle forms,
  string-concat bootstraps, some framework proxies) have names like
  `com.example.Main$$Lambda/0x00007f...` and are not modifiable, so JVMTI
  refuses to retransform them. They appear in `failed.txt`. Classes defined
  *while* the dump runs are captured at definition time and usually do end up
  in the output. This is a JVMTI restriction, not a jvmdump bug.
* **Retransformation has a cost.** It deoptimises the affected methods, so the
  target JVM may pause briefly (typically well under a second for a normal
  application). Nothing is permanently changed.
* **You get the current bytes.** If another tool already redefined or
  retransformed a class, you see the instrumented version.
* **Same user only.** HotSpot only accepts attach requests from the JVM's own
  user. `sudo jvmdump <pid>` works because the client switches its effective
  uid to the JVM's user; the output directory must be writable by that user.
* **Linux + HotSpot only.** OpenJ9 and other JVMs are not supported. `procfs`
  is required.
* A comma in the output path is not supported (the agent option parser uses
  commas as separators).
* **The JDK 21+ "dynamic agent" warning is hidden.** HotSpot writes
  `WARNING: A JVMTI agent has been loaded dynamically` to the *target's* stderr
  after opening the agent library but before `Agent_OnAttach` runs, so no agent
  code can prevent it and `-XX:+EnableDynamicAgentLoading` cannot be turned on
  at runtime. `jvmdump` therefore points the target's stderr at `/dev/null` for
  the few microseconds between `dlopen()` and the attach entry point and
  restores it immediately; the application's own stderr output is unaffected
  outside that window. The agent only does this when HotSpot loads it through
  the attach listener, so `-agentpath` startup and unrelated processes are
  never touched.

---

## Troubleshooting

| Message | Meaning |
| --- | --- |
| `does not look like a HotSpot JVM (no libjvm.so ...)` | Wrong PID, the process is not a HotSpot JVM, or it runs in a container whose `/proc` is not visible. |
| `the JVM did not open its attach socket ...` | The JVM was started with `-XX:+DisableAttachMechanism`, or it is owned by another user. |
| `Dynamic agent loading is not enabled` | Restart the JVM with `-XX:+EnableDynamicAgentLoading`, or preload the agent with `-agentpath:/usr/local/lib/jvmdump/libjvmdump.so=out=/tmp/dump`. |
| `the agent did not write .../dump.log` | The output directory is not writable by the JVM user; check the JVM's own stderr for `jvmdump:` messages. |
| Attach hangs / times out | Raise `--timeout`, or run as the JVM's user instead of root. |
| Socket not where jvmdump looks | Set `JVMD_TMPDIR=/path` to point at the directory that contains `.java_pid<pid>` (custom JVM builds, unusual containers). |

### Loading the agent at startup instead

The same library also implements `Agent_OnLoad`, so it can be attached at JVM
start (no attach permission needed, no dynamic-agent warning):

```bash
java -agentpath:/usr/local/lib/jvmdump/libjvmdump.so=out=/tmp/dump,delay=10 -jar app.jar
```

`delay=N` waits N seconds after `VMInit` before dumping; without it the dump
happens at `VMInit`, i.e. before the application's main class is loaded, which
is only useful to verify the agent works in hardened JVMs.

---

## Development

```bash
make            # builds bin/jvmdump and lib/libjvmdump.so
make test       # end-to-end test against a small demo JVM
sudo make install
sudo make uninstall
make clean
```

Layout:

```
src/main.c      CLI, agent discovery, attach orchestration
src/attach.c    HotSpot dynamic attach protocol (socket, trigger, protocol)
src/agent.c     JVMTI agent: class enumeration, hook, file writing
src/util.c      mkdir -p, glob matching, /proc helpers
tests/          demo application + end-to-end script
```

## License

MIT
