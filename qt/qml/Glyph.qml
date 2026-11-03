import QtQuick
Canvas {
    id: icon
    property string name: "play"
    property color color: "#eeeeee"
    implicitWidth: 24; implicitHeight: 24
    onNameChanged: requestPaint()
    onColorChanged: requestPaint()
    onPaint: {
        let c = getContext("2d"); c.reset(); c.scale(width/24, height/24)
        c.strokeStyle = color; c.fillStyle = color; c.lineWidth = 1.7; c.lineCap = "round"; c.lineJoin = "round"
        function line(p) { c.beginPath(); c.moveTo(p[0],p[1]); for(let i=2;i<p.length;i+=2)c.lineTo(p[i],p[i+1]); c.stroke() }
        if(name==="play") {c.beginPath();c.moveTo(7,4);c.lineTo(20,12);c.lineTo(7,20);c.closePath();c.fill()}
        else if(name==="pause") {c.fillRect(6,4,4,16);c.fillRect(14,4,4,16)}
        else if(name==="back") line([15,4,7,12,15,20])
        else if(name==="close") {line([6,6,18,18]);line([18,6,6,18])}
        else if(name==="plus") {line([12,5,12,19]);line([5,12,19,12])}
        else if(name==="folder") {line([3,19,3,5,9,5,11,8,21,8,21,19,3,19]);line([3,10,21,10])}
        else if(name==="search") {c.beginPath();c.arc(10,10,6,0,Math.PI*2);c.stroke();line([15,15,21,21])}
        else if(name==="grid") {for(let x=4;x<20;x+=10)for(let y=4;y<20;y+=10)c.strokeRect(x,y,6,6)}
        else if(name==="list") {for(let y=6;y<=18;y+=6){line([9,y,21,y]);c.fillRect(3,y-1,2,2)}}
        else if(name==="volume" || name==="mute") {line([3,9,7,9,12,5,12,19,7,15,3,15,3,9]);if(name==="volume"){c.beginPath();c.arc(12,12,6,-0.8,0.8);c.stroke();c.beginPath();c.arc(12,12,10,-0.8,0.8);c.stroke()}else{line([16,9,22,15]);line([22,9,16,15])}}
        else if(name==="full") {line([9,3,3,3,3,9]);line([15,3,21,3,21,9]);line([3,15,3,21,9,21]);line([21,15,21,21,15,21])}
        else if(name==="film") {c.strokeRect(3,4,18,16);line([7,4,7,20]);line([17,4,17,20]);for(let y=8;y<20;y+=4){line([3,y,7,y]);line([17,y,21,y])}}
        else if(name==="rewind" || name==="forward") {c.save();if(name==="forward"){c.translate(24,0);c.scale(-1,1)}c.beginPath();c.arc(12,13,8,-2.2,3.6);c.stroke();line([5,3,5,9,11,9]);c.restore();c.font="bold 7px sans-serif";c.textAlign="center";c.fillText("10",12,16)}
        else if(name==="next" || name==="previous") {c.save();if(name==="previous"){c.translate(24,0);c.scale(-1,1)}c.beginPath();c.moveTo(5,5);c.lineTo(16,12);c.lineTo(5,19);c.closePath();c.fill();line([19,5,19,19]);c.restore()}
        else if(name==="check") line([5,12,10,17,20,6])
    }
}
