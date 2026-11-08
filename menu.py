"""Menu model: a tree of Menus holding Items, navigated gz-style.

Every Menu except the root starts with a "return" item, like gz.
An item whose handler is None is "not mapped yet": it is shown dimmed and
tells the user so when selected. Wiring a feature up later means giving
its item a handler (see the TODOs in ootmenu.pyw)."""


class Item:
    selectable = True  # the cursor can stop here (Info lines are only shown)

    def __init__(self, label, handler=None):
        self.label = label
        self.handler = handler

    @property
    def mapped(self):
        return self.handler is not None

    def text(self):
        return self.label

    def select(self, app):
        if self.handler is None:
            app.status(f"{self.label}: not mapped yet")
        else:
            self.handler(app)

    def adjust(self, app, step):
        pass


class Return(Item):
    def __init__(self):
        super().__init__("return", handler=lambda app: app.go_back())


class Submenu(Item):
    def __init__(self, label, menu):
        super().__init__(label, handler=lambda app: app.enter(menu))
        self.menu = menu

    @property
    def mapped(self):
        return True


class Toggle(Item):
    """Checkbox. get/set are None until the feature is mapped."""

    def __init__(self, label, get=None, set=None, detail=None):
        super().__init__(label, handler=None if set is None else self._flip)
        self.get, self.set, self.detail = get, set, detail

    def _flip(self, app):
        self.set(not self.get())
        app.on_setting_changed()

    def text(self):
        on = self.get is not None and self.get()
        box = f"[{'x' if on else ' '}] "
        if self.detail is None:
            return box + self.label
        try:
            extra = self.detail()
        except Exception:
            extra = "-"
        return f"{box}{self.label:<13} {extra}"


class Choice(Item):
    """'label   < value >'. L or D-Right moves forward, D-Left moves back. `names` optionally maps option
    values to what's shown (the value itself is what get/set use)."""

    def __init__(self, label, options, get, set, names=None):
        super().__init__(label, handler=lambda app: self.adjust(app, 1))
        self.options, self.get, self.set, self.names = options, get, set, names or {}

    def text(self):
        v = self.get()
        return f"{self.label + ' ':<14}< {self.names.get(v, v)} >"

    def adjust(self, app, step):
        v = self.get()
        if v in self.options:
            new = self.options[(self.options.index(v) + step) % len(self.options)]
        else:  # a value that isn't one of the options: start from the first / last
            new = self.options[0] if step > 0 else self.options[-1]
        self.set(new)
        app.on_setting_changed()


class Number(Item):
    """'label   < value >' bound to a live game value.

    D-Left/D-Right change it by `step` (a number, or a function returning one; hold to repeat);
    L runs `on_select` (e.g. fill to max). get() returns None while the value is unavailable."""

    def __init__(self, label, get, set, lo=0, hi=lambda: 999, step=1,
                 fmt=str, on_select=None):
        super().__init__(label, handler=on_select)
        self.get, self.set, self.lo, self.hi, self.step, self.fmt = get, set, lo, hi, step, fmt

    @property
    def mapped(self):
        return True

    def _value(self):
        try:
            return self.get()
        except Exception:
            return None

    def text(self):
        v = self._value()
        return f"{self.label + ' ':<14}" + ("-" if v is None else f"< {self.fmt(v)} >")

    def select(self, app):
        if self.handler and self._value() is not None:
            self.handler(app)

    def adjust(self, app, step):
        v = self._value()
        if v is None:
            app.status(f"{self.label}: not available yet")
            return
        hi = self.hi()
        size = self.step() if callable(self.step) else self.step
        self.set(max(self.lo, min(hi if hi is not None else v, v + step * size)))


class Info(Item):
    """Read-only line whose text is computed each redraw; the cursor skips it."""

    selectable = False

    def __init__(self, fn):
        super().__init__("", handler=lambda app: None)
        self.fn = fn

    def text(self):
        try:
            return self.fn()
        except Exception as e:  # never let a bad readout kill the overlay
            return f"<error: {e}>"


class Menu:
    def __init__(self, title, items, root=False):
        self.title = title
        self.items = items if root else [Return()] + items
        self.cursor = 0

    def move(self, step):
        """Move the cursor, skipping lines that are only shown (labels and readouts)."""
        for _ in range(len(self.items)):
            self.cursor = (self.cursor + step) % len(self.items)
            if self.items[self.cursor].selectable:
                return

    @property
    def current(self):
        return self.items[self.cursor]
