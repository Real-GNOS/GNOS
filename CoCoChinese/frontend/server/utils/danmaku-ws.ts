interface RoomMember {
  peer: any
  id: string
}

const rooms = new Map<string, Set<RoomMember>>()

export function getOrCreateRoom(slug: string): Set<RoomMember> {
  if (!rooms.has(slug)) rooms.set(slug, new Set())
  return rooms.get(slug)!
}

export function joinRoom(slug: string, member: RoomMember) {
  getOrCreateRoom(slug).add(member)
}

export function leaveRoom(slug: string, id: string) {
  const room = rooms.get(slug)
  if (!room) return
  for (const member of room) {
    if (member.id === id) {
      room.delete(member)
      break
    }
  }
  if (room.size === 0) rooms.delete(slug)
}

export function broadcastToRoom(slug: string, data: any, excludeId?: string) {
  const room = rooms.get(slug)
  if (!room) return
  const msg = typeof data === 'string' ? data : JSON.stringify(data)
  for (const member of room) {
    if (member.id !== excludeId) {
      try {
        member.peer.send(msg)
      } catch {}
    }
  }
}
