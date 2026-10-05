import prisma from './prisma'
import { cacheWrap, cacheKey, cacheDel, cacheDelPattern } from './redis'
import { uniqueSlug, generateSlug } from './pg'

export async function findUserByUsername(username: string) {
  return prisma.user.findUnique({ where: { username } })
}

export async function findUserById(id: number) {
  return prisma.user.findUnique({
    where: { id },
    select: { id: true, username: true, role: true, email: true, avatar_url: true, display_name: true, created_at: true },
  })
}

export async function findAllUsers() {
  return prisma.user.findMany({
    select: { id: true, username: true, role: true, email: true, avatar_url: true, created_at: true },
    orderBy: { created_at: 'desc' },
  })
}

export async function getUserProfile(userId: number) {
  const user = await prisma.user.findUnique({
    where: { id: userId },
    include: { profile: true },
  })
  if (!user) return null
  return {
    id: user.id,
    slug: user.slug,
    username: user.username,
    role: user.role,
    email: user.email,
    avatar_url: user.avatar_url,
    display_name: user.display_name,
    created_at: user.created_at,
    updated_at: user.updated_at,
    bio: user.profile?.bio ?? '',
    gender: user.profile?.gender ?? '',
    birthday: user.profile?.birthday ?? null,
    location: user.profile?.location ?? '',
    website: user.profile?.website ?? '',
    followers: user.profile?.followers ?? 0,
    following: user.profile?.following ?? 0,
    likes: user.profile?.likes ?? 0,
  }
}

export async function updateUserProfile(userId: number, data: Record<string, any>) {
  const allowedProfile = ['bio', 'gender', 'location', 'website']
  const allowedUser = ['display_name']
  const profileData: Record<string, any> = {}
  const userData: Record<string, any> = {}
  for (const [k, v] of Object.entries(data)) {
    if (allowedProfile.includes(k)) profileData[k] = v || ''
    if (allowedUser.includes(k)) userData[k] = v || ''
  }
  if (Object.keys(profileData).length) {
    await prisma.userProfile.upsert({
      where: { user_id: userId },
      update: { ...profileData, updated_at: new Date() },
      create: { user_id: userId, ...profileData },
    })
  }
  if (Object.keys(userData).length) {
    await prisma.user.update({
      where: { id: userId },
      data: { ...userData, updated_at: new Date() },
    })
  }
}

export async function getUserStats(userId: number) {
  const [favCount, histCount, videoCount, msgCount, actCount] = await Promise.all([
    prisma.favorite.count({ where: { userId } }),
    prisma.watchHistory.count({ where: { userId } }),
    prisma.video.count({ where: { author: (await prisma.user.findUnique({ where: { id: userId }, select: { username: true } }))?.username ?? '' } }),
    prisma.message.count({ where: { toUserId: userId, isRead: false } }),
    prisma.activity.count({ where: { userId } }),
  ])
  return { favorites: favCount, history: histCount, videos: videoCount, unreadMessages: msgCount, activities: actCount }
}

export async function getUserFavorites(userId: number, folderId?: number) {
  const where: any = { user_id: userId }
  if (folderId) where.folder_id = folderId
  const items = await prisma.favorite.findMany({
    where,
    include: { video: true },
    orderBy: { created_at: 'desc' },
  })
  return items.map(f => ({ id: f.id, created_at: f.created_at, ...f.video }))
}

export async function getFavoriteFolders(userId: number) {
  const folders = await prisma.favoriteFolder.findMany({
    where: { user_id: userId },
    include: { _count: { select: { items: true } } },
    orderBy: { created_at: 'desc' },
  })
  return folders.map(f => ({ ...f, video_count: f._count.items }))
}

export async function getUserHistory(userId: number, limit = 50) {
  return prisma.watchHistory.findMany({
    where: { user_id: userId },
    include: { video: true },
    orderBy: { watched_at: 'desc' },
    take: limit,
  })
}

export async function getUserMessages(userId: number) {
  return prisma.message.findMany({
    where: { to_user_id: userId },
    include: { from: { select: { username: true, avatar_url: true } } },
    orderBy: { created_at: 'desc' },
    take: 100,
  })
}

export async function getActivities(userId?: number, limit = 20) {
  const where = userId ? { user_id: userId } : {}
  return prisma.activity.findMany({
    where,
    include: { user: { select: { username: true, display_name: true, avatar_url: true } } },
    orderBy: { created_at: 'desc' },
    take: limit,
  })
}

export async function getComments(videoSlug: string) {
  return prisma.comment.findMany({
    where: { video_slug: videoSlug },
    include: { user: { select: { avatar_url: true, display_name: true } } },
    orderBy: [{ parent_id: { sort: 'asc', nulls: 'first' } }, { created_at: 'asc' }],
  })
}

export async function addComment(videoSlug: string, userId: number, username: string, avatarUrl: string | null, content: string, parentId?: number) {
  return prisma.comment.create({
    data: { video_slug: videoSlug, user_id: userId, username, avatar_url: avatarUrl, content, parent_id: parentId ?? null },
  })
}

export async function getDanmaku(videoSlug: string) {
  return prisma.danmaku.findMany({
    where: { videoSlug },
    orderBy: { time: 'asc' },
  })
}

export async function addDanmaku(slug: string, userId: number | null, username: string, content: string, time: number, type: string, color: string) {
  return prisma.danmaku.create({
    data: {
      username,
      content,
      time,
      type,
      color,
      video: { connect: { slug } },
      ...(userId ? { user: { connect: { id: userId } } } : {}),
    },
  })
}

export async function getCreatorStats(userId: number) {
  const username = (await prisma.user.findUnique({ where: { id: userId }, select: { username: true } }))?.username
  if (!username) return { total_views: 0, total_likes: 0, total_videos: 0 }
  const videos = await prisma.video.findMany({ where: { author: username } })
  return {
    total_views: videos.reduce((sum, v) => sum + (parseInt(v.watch_volue) || 0), 0),
    total_likes: videos.reduce((sum, v) => sum + (parseInt(v.like_volue) || 0), 0),
    total_videos: videos.length,
  }
}

export async function createBan(userId: number, operatorId: number, type: string, reason: string, durationMinutes?: number) {
  const expiresAt = durationMinutes ? new Date(Date.now() + durationMinutes * 60 * 1000) : null
  return prisma.userBan.create({
    data: { user_id: userId, operator_id: operatorId, type, reason, duration: durationMinutes ?? null, expires_at: expiresAt },
  })
}

export async function getActiveBan(userId: number) {
  return prisma.userBan.findFirst({
    where: {
      user_id: userId,
      is_active: true,
      OR: [{ expires_at: null }, { expires_at: { gt: new Date() } }],
    },
    include: { operator: { select: { username: true } } },
    orderBy: { created_at: 'desc' },
  })
}

export async function getBanList(page = 1, size = 20, status?: string) {
  const where: any = {}
  if (status === 'active') {
    where.is_active = true
    where.OR = [{ expires_at: null }, { expires_at: { gt: new Date() } }]
  } else if (status === 'expired') {
    where.OR = [{ is_active: false }, { expires_at: { not: null } }, { expires_at: { lte: new Date() } }]
  }
  const [total, bans] = await Promise.all([
    prisma.userBan.count({ where }),
    prisma.userBan.findMany({
      where,
      include: {
        user: { select: { username: true, avatar_url: true } },
        operator: { select: { username: true } },
      },
      orderBy: { created_at: 'desc' },
      skip: (page - 1) * size,
      take: size,
    }),
  ])
  return { total, bans }
}

export async function updateBan(banId: number, updates: { is_active?: boolean; reason?: string; duration?: number }) {
  const data: any = { ...updates }
  if (updates.duration !== undefined) {
    data.expires_at = updates.duration ? new Date(Date.now() + updates.duration * 60 * 1000) : null
  }
  data.updated_at = new Date()
  return prisma.userBan.update({ where: { id: banId }, data })
}

export async function deleteBan(banId: number) {
  await prisma.userBan.delete({ where: { id: banId } })
  return { success: true }
}

export async function getBanById(banId: number) {
  return prisma.userBan.findUnique({
    where: { id: banId },
    include: {
      user: { select: { username: true, avatar_url: true } },
      operator: { select: { username: true } },
    },
  })
}

export function toLegacyVideo(v: any) {
  if (!v) return v
  return {
    id: v.id,
    slug: v.slug,
    title: v.title,
    description: v.description,
    author: v.author,
    author_img: v.authorImg,
    image_url: v.imageUrl,
    video_url: v.videoUrl,
    video_type: v.videoType,
    video_time: v.videoTime,
    watch_volue: v.watchVolue,
    like_volue: v.likeVolue,
    watch_people: v.watchPeople,
    recommend: v.recommend,
    introduction: v.introduction,
    category: v.category,
    tags: v.tags,
    order: v.order,
    created_at: v.createdAt,
    updated_at: v.updatedAt,
  }
}

export async function ensureProfile(userId: number) {
  const existing = await prisma.userProfile.findUnique({ where: { user_id: userId } })
  if (!existing) {
    await prisma.userProfile.create({ data: { user_id: userId } })
  }
}

export { generateSlug, uniqueSlug } from './pg'
