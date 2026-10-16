#!/usr/bin/env python3
"""Sync Flycast's VMU images with the e-paper VMU over USB.

  flycast_sync.py list          show Flycast VMUs and what is on the board
  flycast_sync.py push [--all]  copy Flycast VMUs (non-empty ones unless --all) to the board
  flycast_sync.py pull          copy VMUs changed on the board back into Flycast
  flycast_sync.py sync          pull, then push
  flycast_sync.py run NAME      start FLY/NAME on the board (e.g. MK-51048.BIN)
  flycast_sync.py menu          return the board to its launcher

A file is only overwritten when the other side has not changed since the last sync;
otherwise both copies are kept and a conflict is reported. Flycast files are always
backed up (<file>.bak-<time>) before being replaced. Close Flycast before pulling.
"""
import argparse, base64, json, os, re, subprocess, sys, time, zlib

import serial  # pyserial

FLYCAST_DIR = os.path.expanduser('~/.local/share/flycast')
STATE_PATH = os.path.join(FLYCAST_DIR, '.epaper_sync.json')
VMU_BYTES = 128 * 1024
BLOCK = 512


def crc(data):
    return zlib.crc32(data) & 0xFFFFFFFF


# ---------- VMU image parsing ----------

def vmu_files(img):
    """Yields (type, name, dc_description) for each file in a VMU flash image."""
    if len(img) != VMU_BYTES:
        return
    root = 255 * BLOCK
    if img[root:root + 16] != b'\x55' * 16:
        return  # not formatted
    dir_last = int.from_bytes(img[root + 0x4A:root + 0x4C], 'little')
    dir_blocks = int.from_bytes(img[root + 0x4C:root + 0x4E], 'little')
    for blk in range(dir_last, dir_last - dir_blocks, -1):
        for e in range(16):
            ent = img[blk * BLOCK + e * 32:blk * BLOCK + e * 32 + 32]
            if ent[0] not in (0x33, 0xCC):
                continue
            first = int.from_bytes(ent[2:4], 'little')
            header_off = int.from_bytes(ent[0x1A:0x1C], 'little')
            hdr = (first + header_off) * BLOCK
            desc = img[hdr + 16:hdr + 48].split(b'\0')[0].decode('shift_jis', 'replace').strip()
            name = ent[4:16].decode('ascii', 'replace').strip('\0 ')
            yield ('GAME' if ent[0] == 0xCC else 'data', name, desc)


def vmu_title(img, fallback):
    files = list(vmu_files(img))
    for kind, name, desc in files:  # a minigame names the VMU best
        if kind == 'GAME' and desc:
            return desc
    for kind, name, desc in files:
        if desc:
            return desc
    return fallback


def flycast_images():
    """Maps board name -> Flycast path for every VMU image in Flycast's data folder."""
    out = {}
    for f in sorted(os.listdir(FLYCAST_DIR)):
        m = re.fullmatch(r'(.*?)_?vmu_save_([A-D][12])\.bin', f)
        if not m:
            continue
        game, port = m.groups()
        base = (game or 'GLOBAL') + ('' if port == 'A1' else '_' + port)
        name = re.sub(r'[^A-Za-z0-9_-]', '_', base)[:36] + '.BIN'
        out[name] = os.path.join(FLYCAST_DIR, f)
    return out


# ---------- board link ----------

class Board:
    def __init__(self, port):
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, 115200, 0.5
        # Linux raises DTR and RTS on open; leave them there. Lowering DTR before RTS
        # passes through the ESP32-S3's USB reset state and restarts the board.
        self.s.dtr = self.s.rts = True
        self.s.open()
        time.sleep(0.2)
        self.s.reset_input_buffer()

    def send(self, line):
        self.s.write((line + '\n').encode())

    def reply(self, timeout=10):
        """Next protocol line, skipping console log output."""
        end = time.time() + timeout
        while time.time() < end:
            line = self.s.readline().decode('ascii', 'replace').strip()
            if line.startswith('@@'):
                return line[2:]
        raise TimeoutError('no reply from board (is the VMU firmware running?)')

    def list(self):
        self.send('@@LIST')
        files = {}
        while (r := self.reply()) != 'END':
            if r.startswith('ERR'):
                raise RuntimeError(r)
            _, name, size, c = r.split()
            files[name] = (int(size), int(c, 16))
        return files

    def get(self, name, tries=3):
        for _ in range(tries):
            self.send(f'@@GET {name}')
            r = self.reply()
            if r.startswith('ERR'):
                raise RuntimeError(r)
            _, size, c = r.split()
            data = b''
            while (r := self.reply()) != 'END':
                data += base64.b64decode(r[2:])
            if len(data) == int(size) and crc(data) == int(c, 16):
                return data
        raise RuntimeError(f'GET {name} failed')

    def put(self, name, data, tries=3):
        for _ in range(tries):
            self.send(f'@@PUT {name} {len(data)} {crc(data):08x}')
            r = None
            for off in range(0, len(data), 288):
                self.send('@@D ' + base64.b64encode(data[off:off + 288]).decode())
                r = self.reply()  # board acknowledges each line before the next is sent
                if not r.startswith('A '):
                    break
            else:
                r = self.reply(timeout=30)
            if r == 'OK':
                return
            print(f'  retrying {name}: {r}')
        raise RuntimeError(f'PUT {name} failed')


# ---------- commands ----------

def load_state():
    try:
        return json.load(open(STATE_PATH))
    except (OSError, ValueError):
        return {}


def save_state(state):
    json.dump(state, open(STATE_PATH, 'w'), indent=1)


def flycast_running():
    return subprocess.run(['pgrep', '-x', 'flycast'], capture_output=True).returncode == 0


def write_index(board, state):
    lines = [f"{name}|{info['title']}" for name, info in sorted(state.items(), key=lambda kv: kv[1]['title'])]
    board.put('INDEX.TXT', ('\n'.join(lines) + '\n').encode())


def cmd_list(board, state, args):
    on_board = board.list()
    print('Flycast VMUs:')
    for name, path in flycast_images().items():
        img = open(path, 'rb').read()
        files = list(vmu_files(img))
        where = 'on board' if name in on_board else ''
        print(f'  {name:28} {len(files):2} files  {vmu_title(img, "-"):32} {where}')
        for kind, fname, desc in files:
            print(f'      {kind:4} {fname:12} {desc}')
    print('Board FLY/:')
    for name, (size, c) in sorted(on_board.items()):
        print(f'  {name:28} {size:7} bytes  crc {c:08x}')


def cmd_push(board, state, args):
    on_board = board.list()
    changed = False
    for name, path in flycast_images().items():
        img = open(path, 'rb').read()
        if not args.all and not list(vmu_files(img)):
            continue  # empty VMU
        local = crc(img)
        synced = state.get(name, {}).get('crc')
        remote = on_board.get(name, (0, None))[1]
        if remote == local:
            state.setdefault(name, {})['crc'] = local
        elif remote is not None and remote != synced:
            print(f'  CONFLICT {name}: changed on the board since the last sync; run "pull" first')
            continue
        else:
            print(f'  push {name}  ({vmu_title(img, name)})')
            board.put(name, img)
            changed = True
        state[name] = {'crc': local, 'flycast': path, 'title': vmu_title(img, name[:-4])}
    if changed or 'INDEX.TXT' not in on_board:
        write_index(board, state)
    save_state(state)


def cmd_pull(board, state, args):
    if flycast_running() and not args.force:
        sys.exit('Flycast is running and would overwrite its VMU files; close it first (or --force).')
    on_board = board.list()
    stamp = time.strftime('%Y%m%d-%H%M%S')
    for name, info in state.items():
        if name not in on_board or not os.path.exists(info['flycast']):
            continue
        remote = on_board[name][1]
        local_img = open(info['flycast'], 'rb').read()
        local = crc(local_img)
        if remote == local or remote == info['crc']:
            continue  # board unchanged since the last sync
        data = board.get(name)
        if local != info['crc']:
            keep = f"{info['flycast']}.epaper-{stamp}"
            open(keep, 'wb').write(data)
            print(f'  CONFLICT {name}: changed in Flycast and on the board; board copy saved as {keep}')
            continue
        backup = f"{info['flycast']}.bak-{stamp}"
        os.replace(info['flycast'], backup)
        open(info['flycast'], 'wb').write(data)
        info['crc'] = remote
        info['title'] = vmu_title(data, info['title'])
        print(f'  pull {name} -> {info["flycast"]}  (old copy: {os.path.basename(backup)})')
    save_state(state)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('command', choices=['list', 'push', 'pull', 'sync', 'run', 'menu'])
    ap.add_argument('name', nargs='?')
    ap.add_argument('--port', default='/dev/ttyACM0')
    ap.add_argument('--all', action='store_true', help='push empty VMUs too')
    ap.add_argument('--force', action='store_true', help='pull even while Flycast is running')
    args = ap.parse_args()

    board, state = Board(args.port), load_state()
    if args.command == 'list':
        cmd_list(board, state, args)
    elif args.command == 'push':
        cmd_push(board, state, args)
    elif args.command == 'pull':
        cmd_pull(board, state, args)
    elif args.command == 'sync':
        cmd_pull(board, state, args)
        cmd_push(board, state, args)
    elif args.command in ('run', 'menu'):
        if args.command == 'run' and not args.name:
            sys.exit('run needs a NAME, e.g. MK-51048.BIN')
        board.send(f'@@RUN {args.name}' if args.command == 'run' else '@@MENU')
        print(board.reply())


if __name__ == '__main__':
    main()
