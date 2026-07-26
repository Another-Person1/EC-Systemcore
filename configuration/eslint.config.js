import eslint from "@eslint/js";
import tseslint from "typescript-eslint";

export default tseslint.config(
  {
    ignores: ["dist/**", "server.js"]
  },
  eslint.configs.recommended,
  ...tseslint.configs.recommended,
  {
    files: ["**/*.ts"],
    languageOptions: {
      parserOptions: {
        projectService: true,
        tsconfigRootDir: import.meta.dirname
      }
    },
    rules: {
      "@typescript-eslint/no-explicit-any": "error",
      "@typescript-eslint/no-floating-promises": "error"
    }
  },
  {
    files: ["public/app.js"],
    languageOptions: {
      ecmaVersion: 2022,
      sourceType: "script",
      globals: {
        URL: "readonly",
        WebSocket: "readonly",
        clearTimeout: "readonly",
        confirm: "readonly",
        document: "readonly",
        fetch: "readonly",
        Headers: "readonly",
        location: "readonly",
        setTimeout: "readonly",
        window: "readonly"
      }
    }
  }
);
