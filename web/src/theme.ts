import { createTheme } from '@mui/material/styles';

/**
 * Dark, high-contrast, vaguely CRT. The game is white-on-black by nature, so
 * the chrome stays out of its way: near-black surfaces, one cyan accent that
 * matches the player's own paddle, one pink that matches the opponent's.
 */
export const theme = createTheme({
  palette: {
    mode: 'dark',
    background: { default: '#07090d', paper: '#0d1117' },
    primary: { main: '#7ee7ff' },
    secondary: { main: '#ff9de2' },
    success: { main: '#7dffa8' },
    warning: { main: '#ffc878' },
    error: { main: '#ff8080' },
    text: { primary: '#dce9f5', secondary: 'rgba(220,233,245,0.62)' },
  },
  typography: {
    fontFamily: 'ui-monospace, SFMono-Regular, Menlo, Consolas, monospace',
    h1: { fontSize: '2rem', fontWeight: 700, letterSpacing: '0.12em' },
    button: { letterSpacing: '0.1em', fontWeight: 600 },
  },
  shape: { borderRadius: 10 },
  components: {
    MuiPaper: {
      styleOverrides: {
        root: { backgroundImage: 'none', border: '1px solid rgba(126,231,255,0.12)' },
      },
    },
    MuiButton: { defaultProps: { disableElevation: true } },
  },
});
