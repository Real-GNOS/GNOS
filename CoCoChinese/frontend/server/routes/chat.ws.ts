import { defineWebSocketHandler } from 'h3'
import { verifyToken } from '../utils/jwt'
import { queryPg } from '../utils/pg'
import { publishChatMessage, registerOnlineUser, unregisterOnlineUser, subscribeChat, getOnlineUsers, getOnlinePeersForUser } from '../utils/chat-events'

function generateId() {
  return Math.random().toString(36).slice(2, 10)
}

export default defineWebSocketHandler({
  async upgrade(request, context) {
    const url = new URL(request.url)
    const token = url.searchParams.get('token')
    if (!token) {
      return new Response('Unauthorized', { status: 401 })
    }
    const decoded = verifyToken(token)
    if (!decoded || !decoded.userId) {
      return new Response('Unauthorized', { status: 401 })
    }
    context.peer = { userId: parseInt(decoded.userId), username: decoded.username, avatar_url: decoded.avatar_url || '', display_name: decoded.display_name || '' }
  },

  open(peer) {
    if (!peer.context?.userId) return
    const sessionId = generateId()
    peer._sessionId = sessionId

    const unsubscribe = subscribeChat(peer.context.userId, (data: any) => {
      try {
        peer.send(JSON.stringify({ type: 'new_message', data }))
      } catch {}
    })
    peer._unsubscribe = unsubscribe

    registerOnlineUser(peer.context.userId, { peer, id: sessionId })

    const onlineUsers = getOnlineUsers()
    peer.send(JSON.stringify({ type: 'connected', userId: peer.context.userId, onlineUsers }))

    broadcastToContacts(peer.context.userId, { type: 'user_online', userId: peer.context.userId, username: peer.context.username })
  },

  async message(peer, message) {
    if (!peer.context?.userId) return
    let msg
    try {
      msg = JSON.parse(message.text())
    } catch { return }

    switch (msg.type) {
      case 'ping':
        peer.send(JSON.stringify({ type: 'pong' }))
        break
      case 'send_message': {
        const { to_user_id, content, image_url, message_type } = msg
        if (!to_user_id || (!content && !image_url)) return
        try {
          const result = await queryPg(
            `INSERT INTO messages (from_user_id, to_user_id, content, image_url, message_type)
             VALUES ($1, $2, $3, $4, $5) RETURNING id, created_at`,
            [peer.context.userId, to_user_id, content || '', image_url || '', message_type || 'dm']
          )
          const newMsg = {
            id: result.rows[0].id,
            from_user_id: peer.context.userId,
            from_username: peer.context.username,
            from_avatar: peer.context.avatar_url,
            to_user_id,
            content: content || '',
            image_url: image_url || '',
            message_type: message_type || 'dm',
            is_read: false,
            created_at: result.rows[0].created_at,
          }
          publishChatMessage(newMsg)
          peer.send(JSON.stringify({ type: 'message_sent', data: newMsg }))
        } catch (err: any) {
          peer.send(JSON.stringify({ type: 'error', message: '发送失败' }))
        }
        break
      }
      case 'typing': {
        const { to_user_id, is_typing } = msg
        if (!to_user_id) return
        const peers = getOnlinePeersForUser(to_user_id)
        for (const p of peers) {
          try {
            p.send(JSON.stringify({ type: 'typing', from_user_id: peer.context.userId, is_typing: !!is_typing }))
          } catch {}
        }
        break
      }
      case 'mark_read': {
        const { message_ids } = msg
        if (!message_ids || !message_ids.length) return
        try {
          const ids = message_ids.map((id: number) => parseInt(id)).filter((id: number) => !isNaN(id))
          if (ids.length) {
            await queryPg(
              `UPDATE messages SET is_read = true WHERE id IN (${ids.map((_: number, i: number) => '$' + (i + 2)).join(',')}) AND to_user_id = $1`,
              [peer.context.userId, ...ids]
            )
            const peers = getOnlinePeersForUser(msg.from_user_id || peer.context.userId)
            for (const p of peers) {
              try {
                p.send(JSON.stringify({ type: 'read_receipt', read_by: peer.context.userId, message_ids: ids }))
              } catch {}
            }
          }
        } catch {}
        break
      }
    }
  },

  close(peer) {
    if (peer.context?.userId) {
      if (peer._unsubscribe) peer._unsubscribe()
      unregisterOnlineUser(peer.context.userId, peer._sessionId || '')
      broadcastToContacts(peer.context.userId, { type: 'user_offline', userId: peer.context.userId, username: peer.context.username })
    }
  },
})

function broadcastToContacts(userId: number, data: any) {
  const msg = JSON.stringify(data)
  for (const uid of getOnlineUsers()) {
    if (uid === userId) continue
    const peers = getOnlinePeersForUser(uid)
    for (const p of peers) {
      try { p.send(msg) } catch {}
    }
  }
}
