import java.awt.*;
import java.awt.event.*;

public class AWT1 extends Frame {

    private String lastKey = "None";
    private int mouseX = 0;
    private int mouseY = 0;

    public AWT1() {
        // Imposta il titolo della finestra Intuition
        super("Test AWT - AmigaOS 4 Port");
        setSize(400, 300);
        setLocationRelativeTo(null);

        // Gestione della chiusura della finestra dal gadget di Workbench
        addWindowListener(new WindowAdapter() {
            public void windowClosing(WindowEvent we) {
                System.exit(0);
            }
        });

        // Gestione degli input da tastiera
        addKeyListener(new KeyAdapter() {
            public void keyPressed(KeyEvent e) {
                lastKey = KeyEvent.getKeyText(e.getKeyCode());
                repaint(); // Forza il ridisegno della finestra
            }
        });

        // Gestione del movimento del mouse
        addMouseListener(new MouseAdapter() {
            public void mousePressed(MouseEvent e) {
                mouseX = e.getX();
                mouseY = e.getY();
                repaint();
            }
        });
    }

    // Il motore grafico (gestito da Java2D blittato su Intuition)
    public void paint(Graphics g) {
        // Sfondo personalizzato stile Amiga classico (un richiamo al blu/grigio)
        g.setColor(new Color(80, 100, 140));
        g.fillRect(0, 0, getWidth(), getHeight());

        // Disegno di forme geometriche per testare il motore grafico
        g.setColor(Color.ORANGE);
        g.fillOval(50, 60, 80, 80);

        g.setColor(Color.GREEN);
        g.fillRect(250, 60, 80, 80);

        g.setColor(Color.WHITE);
        g.drawRect(20, 40, getWidth() - 40, getHeight() - 60);

        // Visualizzazione dello stato degli input rilevati
        g.setFont(new Font("Monospaced", Font.BOLD, 14));
        g.drawString("Port AmigaOS4 Java 8 Test", 30, 180);
        g.drawString("Last key pressed: " + lastKey, 30, 210);
        g.drawString("Mouse click at: X=" + mouseX + ", Y=" + mouseY, 30, 240);
    }

    public static void main(String[] args) {
        System.out.println("Opening AWT window on AmigaOS 4...");
        AWT1 app = new AWT1();
        app.setVisible(true);
    }
}