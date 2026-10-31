// Mock-up only: Install and Buy buttons say so instead of acting.
const note = Object.assign(document.createElement('div'), { className: 'mock-note', role: 'status' });
document.body.append(note);
let timer;
for (const el of document.querySelectorAll('[data-mock]')) {
  el.addEventListener('click', (e) => {
    e.preventDefault();
    note.textContent = el.dataset.mock;
    note.classList.add('show');
    clearTimeout(timer);
    timer = setTimeout(() => note.classList.remove('show'), 2200);
  });
}
