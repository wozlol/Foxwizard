// Route-card reorder drag-and-drop, ported from Regime Radar's useDragAndDropSimple.js "lift and
// drag" feel: a floating clone follows the cursor while the original card fades to 30% opacity,
// dropping bounces the card, and touch uses a 300ms long-press (with a vibrate) before it starts
// dragging so normal scrolling/tapping still works. Listeners are delegated onto `container` once
// so this survives the container's innerHTML being rebuilt on every route-list re-render.
const transparentImage = new Image();
transparentImage.src = 'data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7';

export function attachDragReorder(container, { itemSelector, onReorder }) {
  const state = {
    draggedIndex: null,
    dragOverIndex: null,
    isDragging: false,
    startX: 0,
    startY: 0,
    dragElement: null,
    clone: null,
    isLongPressing: false,
    lastOverIndex: null,
    scrollY: 0
  };

  function indexOf(el) {
    const item = el.closest(itemSelector);
    return item ? parseInt(item.dataset.routeIndex, 10) : null;
  }

  function elementFor(index) {
    return container.querySelector(`${itemSelector}[data-route-index="${index}"]`);
  }

  function cleanup() {
    if (state.clone) { state.clone.remove(); state.clone = null; }
    if (state.dragElement) {
      state.dragElement.classList.remove('drag-fading');
    }
    container.querySelectorAll(itemSelector).forEach((el) => el.classList.remove('drag-over'));
    // Mirrors Regime Radar's touch drag lock: plain `overflow:hidden` on body alone doesn't
    // reliably stop scrolling on mobile Safari/Chrome (rubber-band/overscroll still gets through
    // and fights the drag gesture) — pinning body to fixed position at the negative scroll offset
    // is what actually holds it still. Has to be undone in the same order it was applied, then the
    // scroll position restored, or the page jumps to the top on every drag.
    document.body.classList.remove('dragging-active');
    document.body.style.overflow = '';
    document.body.style.position = '';
    document.body.style.top = '';
    document.body.style.width = '';
    if (state.scrollY) window.scrollTo(0, state.scrollY);
    state.draggedIndex = null;
    state.dragOverIndex = null;
    state.isDragging = false;
    state.dragElement = null;
    state.lastOverIndex = null;
    state.scrollY = 0;
  }

  function bounce(index) {
    const el = elementFor(index);
    if (!el) return;
    el.classList.add('dropping');
    setTimeout(() => el.classList.remove('dropping'), 400);
  }

  function makeClone(rect) {
    const clone = state.dragElement.cloneNode(true);
    clone.classList.add('route-drag-clone');
    clone.style.position = 'fixed';
    clone.style.top = rect.top + 'px';
    clone.style.left = rect.left + 'px';
    clone.style.width = rect.width + 'px';
    clone.removeAttribute('data-route-index');
    document.body.appendChild(clone);
    return clone;
  }

  // --- Desktop (HTML5 DnD) ---------------------------------------------------------------------
  container.addEventListener('dragstart', (e) => {
    const idx = indexOf(e.target);
    if (idx === null) return;
    if (e.dataTransfer) {
      e.dataTransfer.setDragImage(transparentImage, 0, 0);
      e.dataTransfer.effectAllowed = 'move';
      e.dataTransfer.setData('text/plain', 'reorder');
    }
    state.draggedIndex = idx;
    state.isDragging = true;
    const card = elementFor(idx);
    state.dragElement = card;
    const rect = card.getBoundingClientRect();
    state.startX = e.clientX - rect.left;
    state.startY = e.clientY - rect.top;
    setTimeout(() => card.classList.add('drag-fading'), 0);
    state.clone = makeClone(rect);
  });

  container.addEventListener('dragover', (e) => {
    e.preventDefault();
    const idx = indexOf(e.target);
    if (idx !== null) {
      state.dragOverIndex = idx;
      container.querySelectorAll(itemSelector).forEach((el) => el.classList.remove('drag-over'));
      elementFor(idx)?.classList.add('drag-over');
    }
    if (state.clone) {
      state.clone.style.left = (e.clientX - state.startX) + 'px';
      state.clone.style.top = (e.clientY - state.startY) + 'px';
    }
  });

  container.addEventListener('drop', (e) => {
    e.preventDefault();
    const idx = indexOf(e.target);
    if (state.draggedIndex !== null && idx !== null && idx !== state.draggedIndex) {
      onReorder(state.draggedIndex, idx);
      setTimeout(() => bounce(idx), 50);
    } else if (state.draggedIndex !== null) {
      setTimeout(() => bounce(state.draggedIndex), 50);
    }
    cleanup();
  });

  container.addEventListener('dragend', () => cleanup());

  // --- Touch -------------------------------------------------------------------------------------
  // -webkit-touch-callout:none (CSS) doesn't reliably stop this on every mobile browser (Android
  // Chrome in particular still fires a real contextmenu event on long-press) — blocking the event
  // itself is what actually keeps the native menu from popping up mid-drag.
  container.addEventListener('contextmenu', (e) => {
    if (e.target.closest('.route-drag-handle')) e.preventDefault();
  });

  container.addEventListener('touchstart', (e) => {
    const handle = e.target.closest('.route-drag-handle');
    if (!handle) return;
    const idx = indexOf(handle);
    if (idx === null) return;
    const touch = e.touches[0];
    state.startX = touch.clientX;
    state.startY = touch.clientY;
    state.isLongPressing = true;

    const longPressTimer = setTimeout(() => {
      if (!state.isLongPressing) return;
      state.isLongPressing = false;
      state.isDragging = true;
      state.draggedIndex = idx;
      state.dragElement = elementFor(idx);
      if (navigator.vibrate) { try { navigator.vibrate(50); } catch (err) {} }
      state.scrollY = window.scrollY;
      document.body.classList.add('dragging-active');
      document.body.style.overflow = 'hidden';
      document.body.style.position = 'fixed';
      document.body.style.top = `-${state.scrollY}px`;
      document.body.style.width = '100%';
      const rect = state.dragElement.getBoundingClientRect();
      state.clone = makeClone(rect);
      state.dragElement.classList.add('drag-fading');
    }, 300);

    const cancelIfMoved = (ev) => {
      if (!state.isLongPressing) return;
      const t = ev.touches[0];
      if (Math.abs(t.clientX - state.startX) > 10 || Math.abs(t.clientY - state.startY) > 10) {
        clearTimeout(longPressTimer);
        state.isLongPressing = false;
        document.removeEventListener('touchmove', cancelIfMoved);
        document.removeEventListener('touchend', cancelOnEnd);
      }
    };
    const cancelOnEnd = () => {
      clearTimeout(longPressTimer);
      state.isLongPressing = false;
      document.removeEventListener('touchmove', cancelIfMoved);
      document.removeEventListener('touchend', cancelOnEnd);
    };
    document.addEventListener('touchmove', cancelIfMoved, { passive: true });
    document.addEventListener('touchend', cancelOnEnd, { passive: true });
  }, { passive: true });

  document.addEventListener('touchmove', (e) => {
    if (!state.isDragging || !state.clone) return;
    e.preventDefault();
    const touch = e.touches[0];
    const deltaX = touch.clientX - state.startX;
    const deltaY = touch.clientY - state.startY;
    const rect = state.dragElement.getBoundingClientRect();
    state.clone.style.left = rect.left + deltaX + 'px';
    state.clone.style.top = rect.top + deltaY + 'px';

    state.clone.style.display = 'none';
    const below = document.elementFromPoint(touch.clientX, touch.clientY);
    state.clone.style.display = '';
    if (below) {
      const idx = indexOf(below);
      if (idx !== null && idx !== state.lastOverIndex) {
        state.lastOverIndex = idx;
        state.dragOverIndex = idx;
        container.querySelectorAll(itemSelector).forEach((el) => el.classList.remove('drag-over'));
        elementFor(idx)?.classList.add('drag-over');
      }
    }
  }, { passive: false });

  function endTouchDrag() {
    if (!state.isDragging) return;
    if (state.draggedIndex !== null && state.dragOverIndex !== null && state.draggedIndex !== state.dragOverIndex) {
      onReorder(state.draggedIndex, state.dragOverIndex);
      setTimeout(() => bounce(state.dragOverIndex), 50);
    } else if (state.draggedIndex !== null) {
      setTimeout(() => bounce(state.draggedIndex), 50);
    }
    cleanup();
  }
  document.addEventListener('touchend', endTouchDrag);
  document.addEventListener('touchcancel', endTouchDrag);
}
