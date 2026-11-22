"""Script builtins against retail: python tools/builtins <command>   (needs Python 3 with capstone, and game/t5/)

  triage              compare the MP exe's builtins with the SP exe's and rewrite status.tsv's triage column
  show <name>         a builtin's function in the two exes, side by side (a script name, as in status.tsv)
  show <mp> <sp>      any two functions, by their addresses (hex); -a shows every line
  scripts             write the zombie maps' scripts to build/builtins/scripts, to grep
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def main(argv):
    if not argv or argv[0] in ("-h", "--help", "help"):
        print(__doc__)
        return 0
    cmd, args = argv[0], [a for a in argv[1:] if a != "-a"]
    if cmd == "triage":
        import triage
        print(triage.summary(triage.run()))
    elif cmd == "scripts":
        import scripts
        from paths import OUT
        scripts.called_builtins(dump=OUT + "/scripts")
        print("the scripts are in", OUT + "/scripts")
    elif cmd == "show" and args:
        import show
        todo = []
        if len(args) == 2 and all(len(a) >= 6 for a in args):
            todo.append((int(args[0], 16), int(args[1], 16)))
        else:
            from paths import STATUS
            for ln in open(STATUS, encoding="utf-8").read().split("\n")[1:]:
                c = ln.split("\t")
                if len(c) > 6 and c[2] == args[0] and c[6]:
                    print("%s %s %s: %s, triage %s" % (c[0], c[1], c[2], c[4], c[7]))
                    todo.append((int(c[5], 16), int(c[6], 16)))
        for a, b in todo:
            print(show.show(a, b, "-a" in argv))
    else:
        print(__doc__)
        return 1
    return 0


sys.exit(main(sys.argv[1:]))
