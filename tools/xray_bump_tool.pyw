"""Small Windows GUI for batch-generating X-Ray Engine bump textures."""

from __future__ import annotations

import os
import queue
import shutil
import sys
import tempfile
import threading
from dataclasses import replace
from pathlib import Path
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from generate_stsoc_xray_bumps import (
    Profile,
    choose_profile,
    output_paths,
    process_texture,
)


APP_TITLE = "X-Ray Bump Tool"
APP_VERSION = "1.0.0"
DEFAULT_ROOT = Path(r"D:\STSoC 3.0\gamedata\textures\stsoc")
REFLECTION_FACTORS = {
    "Умеренные": 0.82,
    "Повышенные": 1.00,
    "Сильные": 1.18,
}
RELIEF_FACTORS = {
    "Мягкий": 0.72,
    "Средний": 1.00,
    "Выраженный": 1.28,
}
REFLECTIVE_PROFILES = {"metal", "ammo", "watch"}


def source_textures(folder: Path) -> list[Path]:
    return sorted(
        path
        for path in folder.rglob("*.dds")
        if not path.stem.lower().endswith("_bump")
        and not path.stem.lower().endswith("_bump#")
    )


def outputs_exist(source: Path) -> bool:
    return all(path.exists() for path in output_paths(source))


def adjusted_profile(relative: Path, reflection_name: str, relief_name: str) -> Profile:
    profile = choose_profile(relative)
    reflection_factor = REFLECTION_FACTORS[reflection_name]
    relief_factor = RELIEF_FACTORS[relief_name]
    updates: dict[str, float] = {
        "normal_strength": profile.normal_strength * relief_factor,
        "height_contrast": profile.height_contrast * (0.85 + 0.15 * relief_factor),
    }
    if profile.name in REFLECTIVE_PROFILES:
        updates.update(
            gloss_base=min(0.98, profile.gloss_base * reflection_factor),
            gloss_luma=min(0.98, profile.gloss_luma * reflection_factor),
            gloss_neutral=min(0.98, profile.gloss_neutral * reflection_factor),
            gloss_min=min(0.98, profile.gloss_min * reflection_factor),
            gloss_max=min(0.98, profile.gloss_max * reflection_factor),
        )
    return replace(profile, **updates)


class BumpToolApp:
    def __init__(self, window: tk.Tk, initial_folder: Path) -> None:
        self.window = window
        self.events: queue.Queue[tuple] = queue.Queue()
        self.cancel_event = threading.Event()
        self.worker: threading.Thread | None = None
        self.close_when_done = False

        window.title(APP_TITLE)
        window.geometry("840x620")
        window.minsize(720, 520)
        window.protocol("WM_DELETE_WINDOW", self.on_close)

        self.folder_var = tk.StringVar(value=str(initial_folder))
        self.reflection_var = tk.StringVar(value="Повышенные")
        self.relief_var = tk.StringVar(value="Средний")
        self.overwrite_var = tk.BooleanVar(value=False)
        self.preserve_thm_var = tk.BooleanVar(value=True)
        self.update_material_var = tk.BooleanVar(value=False)
        self.status_var = tk.StringVar(value="Выберите папку с DDS-текстурами")
        self.progress_var = tk.DoubleVar(value=0)
        self.progress_text_var = tk.StringVar(value="Готово к работе")

        self._build_ui()
        self.window.after(100, self._poll_events)
        self.window.after(250, self.scan)

    def _build_ui(self) -> None:
        style = ttk.Style(self.window)
        if "vista" in style.theme_names():
            style.theme_use("vista")

        outer = ttk.Frame(self.window, padding=14)
        outer.pack(fill="both", expand=True)
        outer.columnconfigure(0, weight=1)
        outer.rowconfigure(5, weight=1)

        ttk.Label(outer, text="Папка с исходными DDS", font=("Segoe UI", 10, "bold")).grid(
            row=0, column=0, sticky="w"
        )
        folder_row = ttk.Frame(outer)
        folder_row.grid(row=1, column=0, sticky="ew", pady=(5, 10))
        folder_row.columnconfigure(0, weight=1)
        self.folder_entry = ttk.Entry(folder_row, textvariable=self.folder_var)
        self.folder_entry.grid(row=0, column=0, sticky="ew")
        self.browse_button = ttk.Button(folder_row, text="Выбрать…", command=self.browse)
        self.browse_button.grid(row=0, column=1, padx=(8, 0))
        self.scan_button = ttk.Button(folder_row, text="Сканировать", command=self.scan)
        self.scan_button.grid(row=0, column=2, padx=(8, 0))

        settings = ttk.LabelFrame(outer, text="Настройки", padding=10)
        settings.grid(row=2, column=0, sticky="ew")
        for column in range(4):
            settings.columnconfigure(column, weight=1 if column in (1, 3) else 0)

        ttk.Label(settings, text="Отражения металла:").grid(row=0, column=0, sticky="w")
        self.reflection_combo = ttk.Combobox(
            settings,
            state="readonly",
            textvariable=self.reflection_var,
            values=list(REFLECTION_FACTORS),
            width=18,
        )
        self.reflection_combo.grid(row=0, column=1, sticky="w", padx=(8, 24))
        ttk.Label(settings, text="Сила рельефа:").grid(row=0, column=2, sticky="w")
        self.relief_combo = ttk.Combobox(
            settings,
            state="readonly",
            textvariable=self.relief_var,
            values=list(RELIEF_FACTORS),
            width=18,
        )
        self.relief_combo.grid(row=0, column=3, sticky="w", padx=(8, 0))

        self.overwrite_check = ttk.Checkbutton(
            settings,
            text="Перезаписывать уже готовые bump-пары",
            variable=self.overwrite_var,
        )
        self.overwrite_check.grid(row=1, column=0, columnspan=2, sticky="w", pady=(10, 0))
        self.preserve_check = ttk.Checkbutton(
            settings,
            text="Сохранять остальные настройки существующих THM",
            variable=self.preserve_thm_var,
        )
        self.preserve_check.grid(row=1, column=2, columnspan=2, sticky="w", pady=(10, 0))
        self.material_check = ttk.Checkbutton(
            settings,
            text="Обновлять material в существующих THM по автоопределению",
            variable=self.update_material_var,
        )
        self.material_check.grid(row=2, column=0, columnspan=4, sticky="w", pady=(6, 0))

        status = ttk.Frame(outer)
        status.grid(row=3, column=0, sticky="ew", pady=(12, 8))
        status.columnconfigure(0, weight=1)
        ttk.Label(status, textvariable=self.status_var).grid(row=0, column=0, sticky="w")
        ttk.Label(status, textvariable=self.progress_text_var).grid(row=0, column=1, sticky="e")
        self.progress = ttk.Progressbar(status, variable=self.progress_var, maximum=100)
        self.progress.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(6, 0))

        actions = ttk.Frame(outer)
        actions.grid(row=4, column=0, sticky="ew", pady=(0, 10))
        self.start_button = ttk.Button(actions, text="Сгенерировать", command=self.start)
        self.start_button.pack(side="left")
        self.cancel_button = ttk.Button(actions, text="Отмена", command=self.cancel, state="disabled")
        self.cancel_button.pack(side="left", padx=(8, 0))
        self.open_button = ttk.Button(actions, text="Открыть папку", command=self.open_folder)
        self.open_button.pack(side="right")

        log_frame = ttk.LabelFrame(outer, text="Журнал", padding=6)
        log_frame.grid(row=5, column=0, sticky="nsew")
        log_frame.columnconfigure(0, weight=1)
        log_frame.rowconfigure(0, weight=1)
        self.log = tk.Text(log_frame, wrap="none", state="disabled", font=("Consolas", 9))
        self.log.grid(row=0, column=0, sticky="nsew")
        scroll = ttk.Scrollbar(log_frame, orient="vertical", command=self.log.yview)
        scroll.grid(row=0, column=1, sticky="ns")
        self.log.configure(yscrollcommand=scroll.set)

    def _folder(self) -> Path:
        return Path(self.folder_var.get().strip().strip('"')).expanduser()

    def _append_log(self, text: str) -> None:
        self.log.configure(state="normal")
        self.log.insert("end", text + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def browse(self) -> None:
        initial = self._folder()
        selected = filedialog.askdirectory(
            title="Выберите папку с DDS-текстурами",
            initialdir=str(initial if initial.exists() else Path.cwd()),
        )
        if selected:
            self.folder_var.set(selected)
            self.scan()

    def scan(self) -> None:
        if self.worker and self.worker.is_alive():
            return
        folder = self._folder()
        if not folder.is_dir():
            self.status_var.set("Папка не найдена")
            return
        try:
            sources = source_textures(folder)
        except OSError as error:
            self.status_var.set(f"Ошибка сканирования: {error}")
            return
        ready = sum(outputs_exist(path) for path in sources)
        pending = len(sources) if self.overwrite_var.get() else len(sources) - ready
        self.status_var.set(
            f"Исходников: {len(sources)} · готовых пар: {ready} · к обработке: {pending}"
        )

    def start(self) -> None:
        folder = self._folder()
        if not folder.is_dir():
            messagebox.showerror(APP_TITLE, "Выбранная папка не существует.")
            return
        sources = source_textures(folder)
        if not sources:
            messagebox.showinfo(APP_TITLE, "В папке не найдено исходных DDS-файлов.")
            return
        if not self.overwrite_var.get():
            sources = [path for path in sources if not outputs_exist(path)]
        if not sources:
            messagebox.showinfo(APP_TITLE, "Все текстуры уже имеют полный bump-комплект.")
            return

        self.cancel_event.clear()
        self.progress_var.set(0)
        self.progress_text_var.set(f"0 / {len(sources)}")
        self._append_log(f"Старт: {folder} ({len(sources)} текстур)")
        self._set_running(True)
        options = {
            "reflection": self.reflection_var.get(),
            "relief": self.relief_var.get(),
            "preserve_thm": self.preserve_thm_var.get(),
            "update_material": self.update_material_var.get(),
        }
        self.worker = threading.Thread(
            target=self._run_batch,
            args=(folder, sources, options),
            daemon=True,
        )
        self.worker.start()

    def _run_batch(self, folder: Path, sources: list[Path], options: dict[str, object]) -> None:
        errors: list[tuple[Path, str]] = []
        processed = 0
        for index, source in enumerate(sources, 1):
            if self.cancel_event.is_set():
                break
            relative = source.relative_to(folder)
            try:
                profile = adjusted_profile(
                    relative,
                    str(options["reflection"]),
                    str(options["relief"]),
                )
                stats = process_texture(
                    source,
                    folder,
                    profile=profile,
                    preserve_source_thm=bool(options["preserve_thm"]),
                    update_existing_material=bool(options["update_material"]),
                )
                processed += 1
                message = (
                    f"OK  {relative}  [{stats['profile']}; gloss {stats['gloss_mean']:.0f}]"
                )
            except Exception as error:  # keep a long batch moving after a bad file
                errors.append((relative, str(error)))
                message = f"ERR {relative}: {error}"
            self.events.put(("progress", index, len(sources), message))
        self.events.put(("done", processed, errors, self.cancel_event.is_set()))

    def _poll_events(self) -> None:
        try:
            while True:
                event = self.events.get_nowait()
                if event[0] == "progress":
                    _, current, total, message = event
                    self.progress_var.set(current / total * 100)
                    self.progress_text_var.set(f"{current} / {total}")
                    self._append_log(message)
                elif event[0] == "done":
                    _, processed, errors, cancelled = event
                    self._finish(processed, errors, cancelled)
        except queue.Empty:
            pass
        self.window.after(100, self._poll_events)

    def _finish(self, processed: int, errors: list[tuple[Path, str]], cancelled: bool) -> None:
        if self.close_when_done:
            self.window.destroy()
            return
        self._set_running(False)
        if cancelled:
            self._append_log(f"Остановлено. Успешно обработано: {processed}; ошибок: {len(errors)}")
            self.progress_text_var.set("Остановлено")
        else:
            self._append_log(f"Завершено. Успешно: {processed}; ошибок: {len(errors)}")
            self.progress_text_var.set("Завершено")
            if errors:
                messagebox.showwarning(
                    APP_TITLE,
                    f"Обработка завершена. Успешно: {processed}. Ошибок: {len(errors)}.\n"
                    "Подробности находятся в журнале.",
                )
            else:
                messagebox.showinfo(APP_TITLE, f"Готово. Обработано текстур: {processed}.")
        self.scan()

    def _set_running(self, running: bool) -> None:
        normal_state = "disabled" if running else "normal"
        readonly_state = "disabled" if running else "readonly"
        for widget in (
            self.folder_entry,
            self.browse_button,
            self.scan_button,
            self.start_button,
            self.open_button,
            self.overwrite_check,
            self.preserve_check,
            self.material_check,
        ):
            widget.configure(state=normal_state)
        self.reflection_combo.configure(state=readonly_state)
        self.relief_combo.configure(state=readonly_state)
        self.cancel_button.configure(state="normal" if running else "disabled")

    def cancel(self) -> None:
        if self.worker and self.worker.is_alive():
            self.cancel_event.set()
            self.cancel_button.configure(state="disabled")
            self.progress_text_var.set("Остановка после текущей текстуры…")

    def open_folder(self) -> None:
        folder = self._folder()
        if folder.is_dir():
            os.startfile(folder)  # type: ignore[attr-defined]

    def on_close(self) -> None:
        if self.worker and self.worker.is_alive():
            if not messagebox.askyesno(APP_TITLE, "Остановить обработку и закрыть программу?"):
                return
            self.close_when_done = True
            self.cancel_event.set()
            self.cancel_button.configure(state="disabled")
            self.progress_text_var.set("Завершение текущей текстуры…")
            return
        self.window.destroy()


def self_test(folder: Path) -> int:
    if not folder.is_dir():
        if sys.stdout is not None:
            print(f"Folder does not exist: {folder}")
        return 2
    sources = source_textures(folder)
    if not sources:
        if sys.stdout is not None:
            print(f"No source DDS files found in {folder}")
        return 3
    sample = sources[0]
    profile = adjusted_profile(sample.relative_to(folder), "Повышенные", "Средний")
    assert profile.normal_strength >= 0
    assert len(output_paths(sample)) == 4
    if sys.stdout is not None:
        print(f"Self-test OK: {len(sources)} source DDS files; first profile={profile.name}")
    return 0


def generation_self_test(source: Path) -> int:
    if not source.is_file():
        return 2
    with tempfile.TemporaryDirectory(prefix="xray_bump_exe_test_") as temp:
        root = Path(temp)
        test_source = root / source.name
        shutil.copy2(source, test_source)
        profile = adjusted_profile(test_source.relative_to(root), "Повышенные", "Средний")
        process_texture(
            test_source,
            root,
            profile=profile,
            preserve_source_thm=True,
        )
        if not all(path.exists() and path.stat().st_size > 0 for path in output_paths(test_source)):
            return 3
    return 0


def main() -> None:
    args = sys.argv[1:]
    initial_folder = DEFAULT_ROOT
    if args and args[0] == "--self-test":
        folder = Path(args[1]) if len(args) > 1 else initial_folder
        raise SystemExit(self_test(folder))
    if args and args[0] == "--self-test-generate":
        if len(args) < 2:
            raise SystemExit(2)
        raise SystemExit(generation_self_test(Path(args[1])))
    if args and args[0] == "--smoke-gui":
        window = tk.Tk()
        window.withdraw()
        BumpToolApp(window, initial_folder)
        window.update_idletasks()
        window.destroy()
        if sys.stdout is not None:
            print("GUI smoke-test OK")
        return
    if args and not args[0].startswith("-"):
        initial_folder = Path(args[0])

    window = tk.Tk()
    BumpToolApp(window, initial_folder)
    window.mainloop()


if __name__ == "__main__":
    main()
