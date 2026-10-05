import { EventEmitter } from 'events'

const emitter = new EventEmitter()
emitter.setMaxListeners(200)

export function publishDanmaku(slug: string, data: any) {
  emitter.emit(`danmaku:${slug}`, data)
}

export function subscribeDanmaku(slug: string, callback: (data: any) => void) {
  emitter.on(`danmaku:${slug}`, callback)
  return () => { emitter.off(`danmaku:${slug}`, callback) }
}
