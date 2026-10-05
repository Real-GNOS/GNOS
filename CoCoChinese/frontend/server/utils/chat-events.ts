import { EventEmitter } from 'events'

const emitter = new EventEmitter()
emitter.setMaxListeners(500)

export function publishChatMessage(data: any) {
  const userIds = [data.from_user_id, data.to_user_id]
  for (const uid of userIds) {
    emitter.emit(`chat:${uid}`, data)
  }
}

export function subscribeChat(userId: number, callback: (data: any) => void) {
  emitter.on(`chat:${userId}`, callback)
  return () => { emitter.off(`chat:${userId}`, callback) }
}

const onlineUsers = new Map<number, Set<{ peer: any; id: string }>>()

export function isUserOnline(userId: number): boolean {
  const sessions = onlineUsers.get(userId)
  return !!sessions && sessions.size > 0
}

export function getOnlineUsers(): number[] {
  return [...onlineUsers.entries()].filter(([, s]) => s.size > 0).map(([id]) => id)
}

export function registerOnlineUser(userId: number, session: { peer: any; id: string }) {
  if (!onlineUsers.has(userId)) onlineUsers.set(userId, new Set())
  onlineUsers.get(userId)!.add(session)
  emitter.emit('online:change', { userId, online: true })
}

export function unregisterOnlineUser(userId: number, sessionId: string) {
  const sessions = onlineUsers.get(userId)
  if (!sessions) return
  for (const s of sessions) {
    if (s.id === sessionId) {
      sessions.delete(s)
      break
    }
  }
  if (sessions.size === 0) {
    onlineUsers.delete(userId)
    emitter.emit('online:change', { userId, online: false })
  }
}

export function subscribeOnlineStatus(callback: (data: { userId: number; online: boolean }) => void) {
  emitter.on('online:change', callback)
  return () => { emitter.off('online:change', callback) }
}

export function getOnlinePeersForUser(userId: number): any[] {
  const sessions = onlineUsers.get(userId)
  if (!sessions) return []
  return [...sessions].map(s => s.peer)
}
