export default defineNuxtConfig({
  devtools: { enabled: true },

  vite: {
    resolve: {
      alias: {
        '#app-manifest': 'mocked-exports/empty',
      },
    },
  },

  css: [
    'bootstrap/dist/css/bootstrap.css',
    '~/assets/css/common/style.css',
    '~/assets/css/common/nav.css',
    '~/assets/css/common/videosLine.css',
    '~/assets/css/common/carousel.css',
    '~/assets/css/common/footer.css',
    '~/assets/css/font-awesome.css',
    '~/assets/css/base.css',
    '~/assets/css/pageStyle/index.css',
  ],

  nitro: {
    experimental: {
      openAPI: true,
    },
  },

  experimental: {
    websocket: true,
  },

  app: {
    head: {
      title: 'Cocokalo',
      meta: [
        { charset: 'utf-8' },
        { name: 'viewport', content: 'width=device-width, initial-scale=1' },
      ],
      link: [
        { rel: 'icon', type: 'image/x-icon', href: '/favicon.ico' },
      ],
    },
  },

  compatibilityDate: '2026-06-27',
})
