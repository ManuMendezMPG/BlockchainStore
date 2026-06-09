# /unreal — Juego de Unreal Engine (placeholder)

Esta carpeta es un **placeholder**. El proyecto real de Unreal Engine **vive en
Windows** y se sincronizará aquí.

## Por qué no está el proyecto aquí todavía

- Unreal Engine y sus herramientas (compilación C++, editor) corren de forma nativa
  en **Windows**. El proyecto se desarrolla allí.
- Los proyectos de Unreal generan muchos artefactos pesados y regenerables
  (`Binaries/`, `Intermediate/`, `Saved/`, `DerivedDataCache/`) que **no** deben
  versionarse. Ya están excluidos en el `.gitignore` raíz.

## Qué se sincroniza aquí

Cuando el proyecto se traiga a este repo, debería contener solo lo versionable:

```
unreal/
├── <NombreProyecto>.uproject
├── Config/          # .ini de configuración del proyecto
├── Content/         # Assets (.uasset, .umap) — considera Git LFS si pesan
├── Source/          # Código C++ del juego (si aplica)
└── Plugins/         # Plugins propios
```

NO se sincronizan (ignorados por git): `Binaries/`, `Intermediate/`, `Saved/`,
`DerivedDataCache/`, ni los ficheros de solución del IDE (`.sln`, `.vcxproj`, `.vs/`).

## Integración con el resto del proyecto

El juego se comunica con el **puente local** (`/bridge`) por HTTP/WebSocket en
`localhost` para iniciar compras y leer el inventario on-chain del jugador.
Ver el README raíz para el flujo completo.

## Recomendaciones de sincronización

- Si los assets de `Content/` crecen mucho, configura **Git LFS** para los binarios.
- Mantén `.uproject`, `Config/`, `Source/` y `Content/` bajo control de versiones;
  deja que git ignore el resto (ya configurado).
