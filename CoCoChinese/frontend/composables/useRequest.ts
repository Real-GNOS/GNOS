import type { ApiResponse } from '~/types'

interface RequestOptions {
  method?: 'GET' | 'POST' | 'PUT' | 'PATCH' | 'DELETE'
  body?: any
  params?: Record<string, any>
  headers?: Record<string, string>
  onError?: (error: any) => void
}

export function useRequest() {
  const { user } = useUser()
  const toast = useToast()

  async function request<T = any>(url: string, options: RequestOptions = {}): Promise<ApiResponse<T>> {
    const { method = 'GET', body, params, headers = {}, onError } = options

    const fetchOptions: any = {
      method,
      headers: {
        'Content-Type': 'application/json',
        ...headers,
      },
    }

    if (body && method !== 'GET') {
      fetchOptions.body = body
    }

    let fullUrl = url
    if (params) {
      const searchParams = new URLSearchParams()
      Object.entries(params).forEach(([key, value]) => {
        if (value !== undefined && value !== null && value !== '') {
          searchParams.append(key, String(value))
        }
      })
      const qs = searchParams.toString()
      if (qs) fullUrl += `?${qs}`
    }

    try {
      const res = await $fetch<ApiResponse<T>>(fullUrl, fetchOptions)
      return res
    } catch (error: any) {
      const message = error?.data?.message || error?.message || '请求失败'

      if (onError) {
        onError(error)
      } else if (error?.statusCode === 401) {
        toast.error('登录已过期，请重新登录')
        if (user.value && window.location.pathname.startsWith('/admin')) {
          navigateTo('/admin/login')
        }
      } else if (error?.statusCode === 403) {
        toast.error(error?.data?.message || '权限不足')
      } else if (error?.statusCode === 429) {
        toast.warning('请求过于频繁')
      }

      throw error
    }
  }

  function get<T = any>(url: string, params?: Record<string, any>) {
    return request<T>(url, { method: 'GET', params })
  }

  function post<T = any>(url: string, body?: any) {
    return request<T>(url, { method: 'POST', body })
  }

  function put<T = any>(url: string, body?: any) {
    return request<T>(url, { method: 'PUT', body })
  }

  function del<T = any>(url: string, body?: any) {
    return request<T>(url, { method: 'DELETE', body })
  }

  function patch<T = any>(url: string, body?: any) {
    return request<T>(url, { method: 'PATCH', body })
  }

  return { request, get, post, put, del, patch }
}
