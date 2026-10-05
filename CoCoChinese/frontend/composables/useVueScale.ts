import { ref, onMounted, onUnmounted, type Ref } from 'vue'

export interface VueScaleConfig {
  deviseW?: number
  deviseH?: number
  center?: 'middle' | 'top'
  scroll?: boolean
  type?: 'scale' | 'scalePC' | 'roll' | 'rollH5' | 'rotate' | 'rotateH5' | 'show'
  box?: string
  scalePc?: string
  callBack?: (info: VueScaleInfo) => void
}

export interface VueScaleInfo extends VueScaleConfig {
  innerWidth?: number
  innerHeight?: number
  scale?: number
  zoom?: number
  hideHeight?: number
  hideWidth?: number
  showHeight?: number
  showWidth?: number
}

const DEFAULT_CONFIG: Required<VueScaleConfig> = {
  deviseW: 750,
  deviseH: 1508,
  center: 'middle',
  scroll: false,
  type: 'scale',
  box: '.rotate-box',
  scalePc: '',
  callBack: () => {}
}

export function useVueScale(config: VueScaleConfig = {}) {
  const scaleInfo: Ref<VueScaleInfo> = ref({ ...DEFAULT_CONFIG, ...config })
  let timer: ReturnType<typeof setTimeout> | null = null

  function getScale() {
    setTimeout(() => {
      document.body.style.opacity = '1'
    }, 0)
    document.body.style.opacity = '0'

    const boxList = document.querySelectorAll(scaleInfo.value.box!)
    scaleInfo.value.innerWidth = window.innerWidth
    scaleInfo.value.innerHeight = window.innerHeight

    switch (scaleInfo.value.type) {
      case 'scale': {
        if (!scaleInfo.value.center) scaleInfo.value.center = 'middle'

        for (let index = 0; index < boxList.length; index++) {
          const scaleBox = boxList[index] as HTMLElement

          if ((scaleInfo.value.innerWidth! / scaleInfo.value.innerHeight!) < 1) {
            const scale = scaleInfo.value.innerWidth! / scaleInfo.value.deviseW!
            scaleInfo.value.scale = scale
            scaleBox.style.width = scaleInfo.value.deviseW + 'px'

            if (scaleInfo.value.scroll) {
              scaleBox.style.height = scaleInfo.value.innerHeight! / scaleInfo.value.scale + 'px'
              scaleBox.style.overflow = 'auto'
            } else {
              scaleBox.style.height = scaleInfo.value.deviseH + 'px'
            }

            scaleInfo.value.hideHeight = (scaleInfo.value.innerHeight! - scaleInfo.value.deviseH! * scale) / 2 / scale

            if (scaleInfo.value.center === 'middle') {
              scaleBox.style.transform = `scale(${scale}, ${scale}) translate(0, ${scaleInfo.value.hideHeight}px)`
            } else {
              scaleBox.style.transform = `scale(${scale}, ${scale})`
            }

            scaleBox.style.transformOrigin = '0px 0px 0px'
            scaleInfo.value.showHeight = scaleInfo.value.innerHeight! / scaleInfo.value.scale
            scaleInfo.value.showWidth = scaleInfo.value.innerWidth! / scaleInfo.value.scale
          } else {
            document.body.classList.add('pc')
            const scale = parseFloat((scaleInfo.value.innerHeight! / scaleInfo.value.deviseH!).toFixed(2))
            scaleBox.style.width = scaleInfo.value.deviseW + 'px'
            scaleBox.style.height = scaleInfo.value.deviseH + 'px'
            scaleBox.style.overflow = 'hidden'
            scaleBox.style.transform = `scale(${scale}, ${scale}) translate(${(scaleInfo.value.innerWidth! - scaleInfo.value.deviseW! * scale) / 2 / scale}px, 0)`
            scaleBox.style.transformOrigin = '0 0 0'
            if (scaleInfo.value.scroll) {
              scaleBox.style.overflow = 'auto'
            }
          }
        }

        if (scaleInfo.value.scalePc && (scaleInfo.value.innerWidth! / scaleInfo.value.innerHeight!) > 1) {
          const scaleListPC = document.querySelectorAll(scaleInfo.value.scalePc)
          for (let index = 0; index < scaleListPC.length; index++) {
            const scaleBox = scaleListPC[index] as HTMLElement
            const screenScale = window.innerWidth / window.innerHeight
            const deviseScale = scaleInfo.value.deviseW! / scaleInfo.value.deviseH!
            const scale = screenScale < deviseScale ? window.innerWidth / scaleInfo.value.deviseW! : window.innerHeight / scaleInfo.value.deviseH!
            scaleBox.style.width = scaleInfo.value.deviseW + 'px'
            scaleBox.style.height = scaleInfo.value.deviseH + 'px'
            scaleInfo.value.zoom = scale

            if (navigator.userAgent.indexOf('Edge') > -1) {
              scaleBox.style.transform = `scale(${scaleInfo.value.zoom}, ${scaleInfo.value.zoom}) translate(0, 0)`
            } else {
              ;(scaleBox.style as any).zoom = scaleInfo.value.zoom
            }

            scaleBox.style.transformOrigin = 'center'
            scaleBox.style.position = 'absolute'
            scaleBox.style.left = '-50%'
            scaleBox.style.right = '-50%'
            scaleBox.style.top = '-50%'
            scaleBox.style.bottom = '-50%'
            scaleBox.style.margin = 'auto'
          }
        }
        break
      }

      case 'scalePC': {
        const scaleH = scaleInfo.value.innerHeight! / scaleInfo.value.deviseH!
        const scaleW = scaleInfo.value.innerWidth! / scaleInfo.value.deviseW!
        scaleInfo.value.scale = parseFloat((scaleH > scaleW ? scaleH : scaleW).toFixed(2))

        for (let index = 0; index < boxList.length; index++) {
          const scaleBox = boxList[index] as HTMLElement
          scaleBox.parentNode && (scaleBox.parentNode as HTMLElement).setAttribute('style', 'width:100%;height:100%;overflow:hidden')
          scaleBox.style.width = scaleInfo.value.deviseW + 'px'
          scaleBox.style.height = scaleInfo.value.deviseH + 'px'
          scaleBox.style.overflow = 'hidden'
          scaleBox.style.transform = `scale(${scaleInfo.value.scale}, ${scaleInfo.value.scale}) translate(${(scaleInfo.value.innerWidth! - scaleInfo.value.deviseW! * scaleInfo.value.scale) / 2 / scaleInfo.value.scale}px, ${(scaleInfo.value.innerHeight! - scaleInfo.value.deviseH! * scaleInfo.value.scale) / 2 / scaleInfo.value.scale}px)`
          scaleBox.style.transformOrigin = '0 0 0'
          if (scaleInfo.value.scroll) {
            scaleBox.style.overflow = 'auto'
          }
        }
        break
      }

      case 'roll': {
        for (let index = 0; index < boxList.length; index++) {
          const scaleBox = boxList[index] as HTMLElement
          const scale = window.innerWidth / scaleInfo.value.deviseW!
          scaleBox.style.width = scaleInfo.value.deviseW + 'px'
          scaleInfo.value.zoom = scale

          if (navigator.userAgent.indexOf('Edge') > -1) {
            scaleBox.style.transform = `scale(${scaleInfo.value.zoom}, ${scaleInfo.value.zoom}) translate(0, 0)`
          } else {
            ;(scaleBox.style as any).zoom = scaleInfo.value.zoom
          }

          scaleBox.style.transformOrigin = 'center'
          scaleBox.style.margin = '0 auto'
        }
        break
      }

      case 'rollH5': {
        for (let index = 0; index < boxList.length; index++) {
          const scaleBox = boxList[index] as HTMLElement
          const scale = (window as any).isRotate
            ? window.innerWidth / scaleInfo.value.deviseW!
            : window.innerHeight / scaleInfo.value.deviseW!
          scaleBox.style.height = scaleInfo.value.deviseW + 'px'
          scaleInfo.value.zoom = scale

          if (navigator.userAgent.indexOf('Edge') > -1) {
            scaleBox.style.transform = `scale(${scaleInfo.value.zoom}, ${scaleInfo.value.zoom}) translate(0, 0)`
          } else {
            ;(scaleBox.style as any).zoom = scaleInfo.value.zoom
          }

          scaleBox.style.transformOrigin = 'left top'
          scaleBox.style.margin = '0 auto'
        }
        break
      }

      case 'rotate': {
        for (let index = 0; index < boxList.length; index++) {
          const rotateBox = boxList[index] as HTMLElement
          if ((scaleInfo.value.innerWidth! / scaleInfo.value.innerHeight!) < 1) {
            rotateBox.style.transform = `rotate(90deg) translate(0, ${-scaleInfo.value.innerWidth!}px)`
            rotateBox.style.height = scaleInfo.value.innerWidth + 'px'
            rotateBox.style.width = scaleInfo.value.innerHeight + 'px'
            ;(window as any).isRotate = true
            rotateBox.style.transformOrigin = '0px 0px 0px'
          } else {
            rotateBox.style.height = '100%'
            rotateBox.style.width = '100%'
          }
        }
        break
      }

      case 'rotateH5': {
        for (let index = 0; index < boxList.length; index++) {
          const rotateBox = boxList[index] as HTMLElement
          const screenScale = window.innerWidth / window.innerHeight
          if ((scaleInfo.value.innerWidth! / scaleInfo.value.innerHeight!) < 1) {
            const deviseScale = scaleInfo.value.deviseW! / scaleInfo.value.deviseH!
            const scale = screenScale < deviseScale ? window.innerHeight / scaleInfo.value.deviseW! : window.innerWidth / scaleInfo.value.deviseH!
            scaleInfo.value.hideHeight = (scaleInfo.value.innerWidth! - scaleInfo.value.deviseH! * scale) / 2 / scale
            scaleInfo.value.zoom = scale
            rotateBox.style.transform = `scale(${scaleInfo.value.zoom}, ${scaleInfo.value.zoom}) rotate(90deg) translate(0, ${-(scaleInfo.value.innerWidth! / scale - scaleInfo.value.hideHeight)}px)`
            rotateBox.style.height = scaleInfo.value.deviseH + 'px'
            rotateBox.style.width = scaleInfo.value.deviseW + 'px'
            ;(window as any).isRotate = true
            rotateBox.style.transformOrigin = '0px 0px 0px'
          } else {
            const deviseScale = scaleInfo.value.deviseW! / scaleInfo.value.deviseH!
            const scale = screenScale < deviseScale ? window.innerWidth / scaleInfo.value.deviseW! : window.innerHeight / scaleInfo.value.deviseH!
            scaleInfo.value.hideHeight = (scaleInfo.value.innerHeight! - scaleInfo.value.deviseH! * scale) / 2 / scale
            scaleInfo.value.hideWidth = (scaleInfo.value.innerWidth! - scaleInfo.value.deviseW! * scale) / 2 / scale
            scaleInfo.value.zoom = scale
            rotateBox.style.transform = `scale(${scaleInfo.value.zoom}, ${scaleInfo.value.zoom}) translate(${scaleInfo.value.hideWidth}px , ${scaleInfo.value.hideHeight}px)`
            rotateBox.style.height = scaleInfo.value.deviseH + 'px'
            rotateBox.style.width = scaleInfo.value.deviseW + 'px'
            ;(window as any).isRotate = false
            rotateBox.style.transformOrigin = '0px 0px 0px'
          }
        }
        break
      }

      case 'show': {
        for (let index = 0; index < boxList.length; index++) {
          const box = boxList[index] as HTMLElement
          const scale = window.innerWidth / scaleInfo.value.deviseW!
          scaleInfo.value.zoom = scale
          box.style.transform = `scale(${scaleInfo.value.zoom}, ${scaleInfo.value.zoom})`
          box.style.transformOrigin = '0 0'
          ;(box.parentNode as HTMLElement).style.height = scaleInfo.value.deviseH! * scale + 'px'
          ;(box.parentNode as HTMLElement).style.overflow = 'hidden'
          box.style.height = scaleInfo.value.deviseH + 'px'
          box.style.width = scaleInfo.value.deviseW + 'px'
        }
        break
      }
    }

    if (scaleInfo.value.callBack) {
      scaleInfo.value.callBack(scaleInfo.value)
    }
  }

  function refreshGetScale() {
    if (timer) clearTimeout(timer)
    timer = setTimeout(() => {
      getScale()
    }, 300)
  }

  onMounted(() => {
    setTimeout(() => {
      window.addEventListener('resize', refreshGetScale)
      window.addEventListener('pageshow', refreshGetScale)
    }, 100)
    getScale()
  })

  onUnmounted(() => {
    if (timer) clearTimeout(timer)
    window.removeEventListener('resize', refreshGetScale)
    window.removeEventListener('pageshow', refreshGetScale)
  })

  return scaleInfo
}
