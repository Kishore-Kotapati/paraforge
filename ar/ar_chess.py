#!/usr/bin/env python3
"""AR chess viewer for the ParaForge engine.

    python3 ar/ar_chess.py                 # webcam + printed ArUco marker
    python3 ar/ar_chess.py --no-camera     # flat board in a window
    python3 ar/ar_chess.py --selftest      # no window: play 2 moves, save a PNG

Print marker id 0 from the 4x4_50 ArUco dictionary (python3 ar/ar_chess.py
--make-marker writes marker.png) and put it on the table. The board is drawn
around it. Click a piece, then a square, to move. After each engine reply, the
squares of the candidate moves are tinted by the worker thread that searched
them, so you can see how the search was split.

This file only draws and talks to the engine. All chess logic, search and
parallelism live in the C++ engine (paraforge engine), which runs as a child
process and is tested on its own.
"""
import argparse
import os
import subprocess
import sys

try:
    import cv2
    import numpy as np
except ImportError:
    sys.exit("needs OpenCV and numpy: pip install opencv-contrib-python numpy")

BOARD_PX = 640                 # size of the top-down board image
SQ = BOARD_PX // 8
LIGHT, DARK = (222, 227, 232), (120, 140, 160)      # BGR
WORKER_COLORS = [(214, 120, 42), (52, 104, 235), (122, 175, 27), (0, 161, 237),
                 (164, 123, 232), (0, 131, 0), (180, 80, 160), (90, 90, 90)]
MAIN_COLOR = (40, 40, 200)     # first root move, searched alone before the split

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ENGINE = os.path.join(HERE, "..", "paraforge")


class Engine:
    """Talks to `paraforge engine` over stdin/stdout, one command at a time."""

    def __init__(self, path, threads, depth):
        self.proc = subprocess.Popen([path, "engine", "--threads", str(threads),
                                      "--depth", str(depth)],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.proc.stdout.readline()   # "ready ..."

    def _send(self, cmd):
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()

    def command(self, cmd):
        self._send(cmd)
        return self.proc.stdout.readline().strip()

    def board(self):
        self._send("board")
        rows = [self.proc.stdout.readline().split()[1:] for _ in range(8)]
        footer = self.proc.stdout.readline()
        return rows, "white" in footer   # rows[0] is rank 8

    def go(self):
        self._send("go")
        roots, info, best = [], {}, None
        while True:
            parts = self.proc.stdout.readline().split()
            if not parts:
                break
            if parts[:2] == ["info", "root"]:
                # info root <move> score <n> <exact|upper> worker <w>
                roots.append({"move": parts[2], "score": int(parts[4]),
                              "exact": parts[5] == "exact", "worker": int(parts[7])})
            elif parts[:2] == ["info", "score"]:
                info = {"score": int(parts[2]), "nodes": int(parts[4]), "ms": float(parts[6])}
            elif parts[0] == "bestmove":
                best = None if parts[1] == "none" else parts[1]
                break
        return roots, info, best

    def close(self):
        try:
            self._send("quit")
            self.proc.wait(timeout=2)
        except Exception:
            self.proc.kill()


def square_name(col, row):
    """col 0..7 = a..h, row 0..7 from the top (rank 8)."""
    return "abcdefgh"[col] + str(8 - row)


def square_cell(name):
    return "abcdefgh".index(name[0]), 8 - int(name[1])


def render_board(rows, selected=None, roots=(), best=None, status=""):
    img = np.zeros((BOARD_PX, BOARD_PX, 3), np.uint8)
    for r in range(8):
        for c in range(8):
            color = LIGHT if (r + c) % 2 == 0 else DARK
            cv2.rectangle(img, (c * SQ, r * SQ), ((c + 1) * SQ, (r + 1) * SQ), color, -1)

    # Tint each candidate destination by the worker that searched it.
    for info in roots:
        c, r = square_cell(info["move"][2:4])
        w = info["worker"]
        tint = MAIN_COLOR if w < 0 else WORKER_COLORS[w % len(WORKER_COLORS)]
        overlay = img.copy()
        cv2.rectangle(overlay, (c * SQ, r * SQ), ((c + 1) * SQ, (r + 1) * SQ), tint, -1)
        img = cv2.addWeighted(overlay, 0.45, img, 0.55, 0)

    if selected:
        c, r = selected
        cv2.rectangle(img, (c * SQ + 2, r * SQ + 2), ((c + 1) * SQ - 2, (r + 1) * SQ - 2),
                      (0, 200, 255), 3)

    for r in range(8):
        for c in range(8):
            p = rows[r][c]
            if p == ".":
                continue
            center = (c * SQ + SQ // 2, r * SQ + SQ // 2)
            white = p.isupper()
            cv2.circle(img, center, SQ // 3, (245, 245, 245) if white else (30, 30, 30), -1)
            cv2.circle(img, center, SQ // 3, (60, 60, 60), 2)
            cv2.putText(img, p.upper(), (center[0] - 11, center[1] + 11), cv2.FONT_HERSHEY_SIMPLEX,
                        1.0, (30, 30, 30) if white else (235, 235, 235), 2, cv2.LINE_AA)

    if best:
        (c1, r1), (c2, r2) = square_cell(best[:2]), square_cell(best[2:4])
        cv2.arrowedLine(img, (c1 * SQ + SQ // 2, r1 * SQ + SQ // 2),
                        (c2 * SQ + SQ // 2, r2 * SQ + SQ // 2), (0, 0, 220), 4, tipLength=0.25)
    if status:
        cv2.rectangle(img, (0, BOARD_PX - 26), (BOARD_PX, BOARD_PX), (20, 20, 20), -1)
        cv2.putText(img, status, (8, BOARD_PX - 8), cv2.FONT_HERSHEY_SIMPLEX, 0.5,
                    (240, 240, 240), 1, cv2.LINE_AA)
    return img


class Game:
    """Board state on screen plus the click-to-move logic."""

    def __init__(self, engine):
        self.engine = engine
        self.rows, self.white_to_move = engine.board()
        self.selected = None
        self.roots, self.best = [], None
        self.status = "your move (white)"

    def click(self, col, row):
        if not (0 <= col < 8 and 0 <= row < 8):
            return
        if self.selected is None:
            if self.rows[row][col] != ".":
                self.selected = (col, row)
            return
        move = square_name(*self.selected) + square_name(col, row)
        self.selected = None
        self.play(move)

    def play(self, move):
        if self.engine.command("move " + move) != "ok":
            self.status = f"{move} is illegal"
            return False
        self.rows, _ = self.engine.board()
        self.status = "engine thinking..."
        roots, info, best = self.engine.go()
        self.roots, self.best = roots, best
        if best is None:
            self.status = "game over"
            return True
        self.engine.command("move " + best)
        self.rows, _ = self.engine.board()
        workers = sorted({r["worker"] for r in roots if r["worker"] >= 0})
        self.status = (f"engine: {best}  score {info.get('score')}  nodes {info.get('nodes')}  "
                       f"{info.get('ms', 0):.0f} ms  workers used {workers}")
        return True

    def image(self):
        return render_board(self.rows, self.selected, self.roots, self.best, self.status)


def board_homography(corners):
    """Maps board pixels to camera pixels. The marker covers the 2x2 centre squares."""
    src = np.float32([[3 * SQ, 3 * SQ], [5 * SQ, 3 * SQ], [5 * SQ, 5 * SQ], [3 * SQ, 5 * SQ]])
    return cv2.getPerspectiveTransform(src, np.float32(corners))


def run_camera(game, camera_index):
    cap = cv2.VideoCapture(camera_index)
    if not cap.isOpened():
        sys.exit("cannot open camera; try --no-camera")
    detector = cv2.aruco.ArucoDetector(cv2.aruco.getPredefinedDictionary(cv2.aruco.DICT_4X4_50))
    state = {"H": None}

    def on_mouse(event, x, y, *_):
        if event == cv2.EVENT_LBUTTONDOWN and state["H"] is not None:
            # camera pixel -> board pixel through the inverse homography
            pt = cv2.perspectiveTransform(np.float32([[[x, y]]]), np.linalg.inv(state["H"]))
            game.click(int(pt[0, 0, 0] // SQ), int(pt[0, 0, 1] // SQ))

    cv2.namedWindow("ParaForge AR")
    cv2.setMouseCallback("ParaForge AR", on_mouse)
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        corners, ids, _ = detector.detectMarkers(frame)
        if ids is not None and 0 in ids.flatten():
            idx = list(ids.flatten()).index(0)
            state["H"] = board_homography(corners[idx][0])
            board = game.image()
            h, w = frame.shape[:2]
            warped = cv2.warpPerspective(board, state["H"], (w, h))
            mask = cv2.warpPerspective(np.full(board.shape[:2], 255, np.uint8), state["H"], (w, h))
            frame[mask > 0] = cv2.addWeighted(frame, 0.15, warped, 0.85, 0)[mask > 0]
        else:
            state["H"] = None
            cv2.putText(frame, "show marker id 0 (4x4_50)", (20, 40), cv2.FONT_HERSHEY_SIMPLEX,
                        0.9, (0, 0, 255), 2)
        cv2.imshow("ParaForge AR", frame)
        if cv2.waitKey(1) & 0xFF in (27, ord("q")):
            break
    cap.release()


def run_flat(game):
    def on_mouse(event, x, y, *_):
        if event == cv2.EVENT_LBUTTONDOWN:
            game.click(x // SQ, y // SQ)

    cv2.namedWindow("ParaForge")
    cv2.setMouseCallback("ParaForge", on_mouse)
    while True:
        cv2.imshow("ParaForge", game.image())
        if cv2.waitKey(30) & 0xFF in (27, ord("q")):
            break


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", default=DEFAULT_ENGINE)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--depth", type=int, default=4)
    ap.add_argument("--camera", type=int, default=0)
    ap.add_argument("--no-camera", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--make-marker", action="store_true")
    args = ap.parse_args()

    if args.make_marker:
        marker = cv2.aruco.generateImageMarker(
            cv2.aruco.getPredefinedDictionary(cv2.aruco.DICT_4X4_50), 0, 400)
        cv2.imwrite("marker.png", cv2.copyMakeBorder(marker, 40, 40, 40, 40,
                                                     cv2.BORDER_CONSTANT, value=255))
        print("wrote marker.png - print it and place it on the table")
        return

    engine = Engine(args.engine, args.threads, args.depth)
    game = Game(engine)
    try:
        if args.selftest:
            for move in ("e2e4", "d2d4"):
                if not game.play(move):
                    print("could not play", move)
                print(game.status)
            cv2.imwrite("ar_selftest.png", game.image())
            print("wrote ar_selftest.png")
        elif args.no_camera:
            run_flat(game)
        else:
            run_camera(game, args.camera)
    finally:
        engine.close()
        if not args.selftest:
            cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
