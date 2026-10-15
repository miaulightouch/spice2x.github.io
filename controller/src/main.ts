import { createApp } from 'vue';
import App from './App.vue';
import './style.css';

createApp(App).mount('#app');

if ('serviceWorker' in navigator && import.meta.env.PROD) {
  window.addEventListener('load', () => {
    void navigator.serviceWorker.register(new URL('sw.js', document.baseURI).href, { scope: './' })
      .catch(() => { /* The controller also works without offline installation. */ });
  });
}
