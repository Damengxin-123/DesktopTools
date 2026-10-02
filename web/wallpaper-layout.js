/* 图片布局的共享计算：桌面和缩略示意图采用相同的显示规则。 */
(function installWallpaperLayout() {
  "use strict";
  // 根据原图像素和屏幕物理尺寸设置背景；视口可为完整桌面或缩小后的预览。
  function apply(element, source, mode, imageWidth, imageHeight, screenWidth, screenHeight, viewWidth, viewHeight) {
    element.style.backgroundImage = source ? "url(" + JSON.stringify(source) + ")" : "none";
    element.style.backgroundPosition = mode === "tile" ? "left top" : "center";
    element.style.backgroundRepeat = mode === "tile" ? "repeat" : "no-repeat";
    if ((mode === "tile" || mode === "center") && imageWidth > 0 && imageHeight > 0 && screenWidth > 0 && screenHeight > 0) {
      element.style.backgroundSize = (imageWidth * viewWidth / screenWidth) + "px " + (imageHeight * viewHeight / screenHeight) + "px";
    } else {
      element.style.backgroundSize = mode === "stretch" ? "100% 100%" : (mode === "fill" ? "cover" : "contain");
    }
  }
  // 仅暴露布局应用函数，资源读取由原生侧负责。
  window.desktopWallpaperLayout = Object.freeze({ apply });
})();
