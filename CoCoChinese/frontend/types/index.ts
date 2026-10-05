export interface User {
  id: number
  slug: string | null
  username: string
  password?: string
  email: string | null
  role: string
  avatar_url: string | null
  display_name: string
  created_at: string
  updated_at: string
  banner_url?: string
  subscription?: Subscription | null
  profile?: UserProfile
}

export interface UserProfile {
  id: number
  user_id: number
  bio: string
  gender: string
  birthday: string | null
  location: string
  website: string
  followers: number
  following: number
  likes: number
  created_at: string
  updated_at: string
}

export interface Video {
  id: number
  slug: string | null
  title: string
  description: string | null
  author: string | null
  author_img: string | null
  image_url: string | null
  video_url: string | null
  video_url_hls: string | null
  video_url_360p: string | null
  video_url_720p: string | null
  video_url_1080p: string | null
  video_type: string | null
  video_time: string | null
  watch_volue: string
  like_volue: string
  watch_people: string
  recommend: string | null
  introduction: string | null
  category: string | null
  tags: string[]
  order: number
  is_deleted: boolean
  created_at: string
  updated_at: string
  author_username?: string
  author_avatar?: string
}

export interface Episode {
  id: number
  video_id: number
  season: number
  episode: number
  title: string
  video_url: string
  image_url: string | null
  duration: string | null
  sort_order: number
  created_at: string
  updated_at: string
}

export interface Comment {
  id: number
  video_slug: string
  user_id: number
  username: string
  avatar_url: string | null
  content: string
  parent_id: number | null
  images: string
  is_pinned: boolean
  is_featured: boolean
  like_count: number
  created_at: string
  video_title?: string
  user_avatar?: string
  user_display_name?: string
}

export interface Danmaku {
  id: number
  video_slug: string
  user_id: number | null
  username: string
  content: string
  time: number
  type: string
  color: string
  created_at: string
  video_title?: string
}

export interface Carousel {
  id: number
  image_url: string | null
  link: string | null
  title: string | null
  author: string | null
  author_img: string | null
  watch_volue: string
  like_volue: string
  introduction: string | null
  video_time: string | null
  description: string | null
  order: number
  created_at: string
}

export interface Subscription {
  id: number
  user_id: number
  plan: string
  status: string
  provider: string
  provider_id: string | null
  current_period_start: string | null
  current_period_end: string | null
  cancel_at_period_end: boolean
  created_at: string
  updated_at: string
}

export interface Payment {
  id: number
  user_id: number
  amount: number
  currency: string
  status: string
  provider: string
  provider_id: string | null
  plan: string
  created_at: string
  updated_at: string
}

export interface Category {
  id: number
  name: string
  icon: string | null
  order: number
  parent_id: number | null
  created_at: string
}

export interface AuditLog {
  id: number
  user_id: number | null
  action: string
  target_type: string | null
  target_id: number | null
  details: string | null
  ip_address: string | null
  created_at: string
  username?: string
  avatar_url?: string
}

export interface MediaFile {
  id: number
  file_id: string | null
  file_path: string
  name: string | null
  file_type: string | null
  file_size: number | null
  file_hash: string | null
  created_at: string
  updated_at: string
}

export interface ContentReport {
  id: number
  reporter_id: number
  target_type: string
  target_id: number
  reason: string
  description: string | null
  status: string
  handled_by_id: number | null
  handled_at: string | null
  created_at: string
  reporter_name?: string
  reporter_avatar?: string
  handler_name?: string
}

export interface UserBan {
  id: number
  user_id: number
  operator_id: number
  type: string
  reason: string
  duration: number | null
  expires_at: string | null
  is_active: boolean
  created_at: string
  updated_at: string
  target_username?: string
  target_avatar?: string
  operator_name?: string
}

export interface SiteConfig {
  key: string
  value: string
}

export interface DashboardStats {
  totalUsers: number
  totalVideos: number
  totalComments: number
  totalDanmaku: number
  totalViews: number
  pendingReports: number
  activeBans: number
  todayUploads: number
  todayRegisters: number
  storageUsed: number
}

export interface TrendPoint {
  date: string
  views: number
  uploads: number
  registrations: number
}

export interface BlockWord {
  id: number
  word: string
  created_at: string
}

export interface ApiResponse<T = any> {
  success: boolean
  message?: string
  data?: T
  total?: number
  page?: number
  size?: number
  [key: string]: any
}
