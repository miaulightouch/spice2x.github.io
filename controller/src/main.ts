import { createApp, watchEffect } from 'vue';
import App from './App.vue';
import { initializeI18n, locale, t } from './i18n';
import { isEmbeddedBuild } from './mode';
import './style.css';

initializeI18n(navigator.languages?.length ? navigator.languages : [navigator.language]);
watchEffect(() => {
  document.documentElement.lang = locale.value;
  document.title = t('app.title');
  document.querySelector('meta[name=description]')?.setAttribute('content', t('app.description'));
});

createApp(App).mount('#app');

if (!isEmbeddedBuild && 'serviceWorker' in navigator && import.meta.env.PROD) {
  window.addEventListener('load', () => {
    void navigator.serviceWorker.register(new URL('sw.js', document.baseURI).href, { scope: './' })
      .catch(() => { /* The controller also works without offline installation. */ });
  });
}
