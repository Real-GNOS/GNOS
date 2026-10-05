let toastInstance: any = null

export function useToast() {
  function setToast(ref: any) {
    toastInstance = ref
  }

  function success(message: string) { toastInstance?.success(message) }
  function error(message: string) { toastInstance?.error(message) }
  function info(message: string) { toastInstance?.info(message) }
  function warning(message: string) { toastInstance?.warning(message) }

  return { setToast, success, error, info, warning }
}
