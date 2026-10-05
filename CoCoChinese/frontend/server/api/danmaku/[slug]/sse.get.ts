import { createEventStream } from 'h3'
import { subscribeDanmaku } from '../../../utils/danmaku-events'

export default defineEventHandler(async (event) => {
  const slug = getRouterParam(event, 'slug')
  if (!slug) throw createError({ statusCode: 400, message: '缺少参数' })

  const eventStream = createEventStream(event)
  const unsubscribe = subscribeDanmaku(slug, (data) => {
    eventStream.push(JSON.stringify(data))
  })

  eventStream.onClosed(async () => {
    unsubscribe()
  })

  return eventStream.send()
})
