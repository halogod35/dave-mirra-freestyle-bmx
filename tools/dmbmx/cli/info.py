"""dmbmx info <file>: print the leading magic/tag of a game file."""


def register(sub):
    p = sub.add_parser("info", help="show the magic / first chunk tag of a file")
    p.add_argument("file")
    p.set_defaults(func=run)


def run(args):
    with open(args.file, "rb") as f:
        head = f.read(16)
    tag = head[:4]
    printable = tag.decode("ascii") if all(32 <= b < 127 for b in tag) else tag.hex(" ")
    print(f"{args.file}: {printable}  ({len(head)} bytes read) head={head.hex(' ')}")
