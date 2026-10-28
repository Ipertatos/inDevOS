import { twice } from './b.js';
export const A = twice(21);
document.getElementById('r1').textContent = 'static import: ' + A + ' meta=' + import.meta.url.split('/').pop();
